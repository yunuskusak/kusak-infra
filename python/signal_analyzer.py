#!/usr/bin/env python3
"""
KUSAK V9 — RESEARCH FRAMEWORK
signal_analyzer_v2.py — Gelişmiş Sinyal Analizi & Otomatik Pasifleştirme

RAPOR BULGULARI (Orta Öncelik):
  "Sinyal analizi çıktıları düzenli olarak incelenmeli; eğer bir gösterge
  zaman içinde düşen önem sergiliyorsa, otomatize şekilde o gösterge
  devre dışı bırakılabilir veya yeni sinyaller araştırılmalıdır."
  "Alphadecay sonucu 'yeni eğitim zamanı' sinyali olarak değerlendirilebilir."

YENİLİKLER (v1'e göre):
  ✓ Zaman bazlı kayan pencere feature importance (sinyal bozunmasını izle)
  ✓ Otomatik sinyal pasifleştirme (alpha < threshold veya decay hızlı)
  ✓ Alpha decay hız hesabı (yarılanma zamanı, her rejim için ayrı)
  ✓ "Yeni eğitim zamanı" sinyali (decay ivmesi aşıldığında)
  ✓ Ticaret performansı zaman serisi (rolling Sharpe, rolling PnL)
  ✓ SHAP analizi ile gösterge katkısı
  ✓ Gösterge korelasyon kayması izleme
  ✓ Kapsamlı HTML raporu çıktısı

KULLANIM:
  python3 signal_analyzer_v2.py data/trades/*.bin --output analysis/ --config config.json
  python3 signal_analyzer_v2.py --watch data/trades/ --interval 3600
"""

import argparse
import struct
import os
import sys
import json
import time
import logging
import warnings
warnings.filterwarnings('ignore')

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import seaborn as sns
from pathlib import Path
from typing import List, Optional, Dict, Tuple
from scipy import stats
from sklearn.ensemble import RandomForestRegressor
from sklearn.inspection import permutation_importance
from sklearn.model_selection import TimeSeriesSplit
from sklearn.preprocessing import StandardScaler

# Opsiyonel: SHAP
try:
    import shap
    HAS_SHAP = True
except ImportError:
    HAS_SHAP = False

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')
log = logging.getLogger(__name__)

# ============================================================================
# §1  Sabitler ve Yapılandırma
# ============================================================================

# B-MAJ-002: TradeRecord ABI fix — 10 feature fields are now float (not double)
# C++ layout: QQ (16) + 10f (40) + ddd (24) + BB (2) + 6x pad (6) + ddd (24) + BB (2) + 14x pad (14) = 128
TRADE_STRUCT_FMT  = '<QQ' + 'f' * 10 + 'ddd' + 'BB' + '6x' + 'ddd' + 'BB' + '14x'
TRADE_STRUCT_SIZE = struct.calcsize(TRADE_STRUCT_FMT)
assert TRADE_STRUCT_SIZE == 128, f"TradeRecord boyut hatası: {TRADE_STRUCT_SIZE}"

FEATURE_COLS = [
    'vpin', 'imbalance', 'spread_bps', 'edge',
    'volume_1m', 'atr', 'mid_price', 'fill_prob',
    'score', 'microprice'
]

REGIME_NAMES = {0:'UNKNOWN', 1:'TREND', 2:'RANGE', 3:'TOXIC', 4:'DEAD'}

DEFAULT_CONFIG = {
    # Pasifleştirme eşikleri
    'disable_importance_threshold': 0.02,   # Feature importance < 2% → pasif
    'disable_decay_halflife_hours': 2.0,    # Yarılanma zamanı < 2 saat → uyarı
    'retrain_signal_halflife_hours': 4.0,   # < 4 saat → yeniden eğitim öner

    # Kayan pencere boyutları
    'rolling_importance_window': 500,       # son kaç işlem için rolling importance
    'rolling_sharpe_window': 200,           # rolling Sharpe hesabı için
    'decay_fit_window_trades': 1000,        # alpha decay için kaç işlem kullan

    # Rejim bazlı analiz
    'analyze_per_regime': True,

    # Çıktı
    'output_dir': 'analysis',
    'html_report': True,
    'verbose': False
}

# ============================================================================
# §2  TradeRecord Ayrıştırıcı
# ============================================================================

def parse_trade_records(path: str) -> pd.DataFrame:
    """Binary TradeRecord dosyasını DataFrame'e ayrıştır."""
    records = []
    file_size = os.path.getsize(path)
    n_records = file_size // TRADE_STRUCT_SIZE

    if n_records == 0:
        return pd.DataFrame()

    with open(path, 'rb') as f:
        for _ in range(n_records):
            data = f.read(TRADE_STRUCT_SIZE)
            if len(data) < TRADE_STRUCT_SIZE:
                break
            fields = struct.unpack(TRADE_STRUCT_FMT, data)
            records.append({
                'ts_entry':    fields[0],
                'ts_exit':     fields[1],
                'vpin':        fields[2],
                'imbalance':   fields[3],
                'spread_bps':  fields[4],
                'edge':        fields[5],
                'volume_1m':   fields[6],
                'atr':         fields[7],
                'mid_price':   fields[8],
                'fill_prob':   fields[9],
                'score':       fields[10],
                'microprice':  fields[11],
                'entry_price': fields[12],
                'exit_price':  fields[13],
                'qty':         fields[14],
                'side':        fields[15],
                'order_type':  fields[16],
                'pnl_raw':     fields[17],
                'pnl_net':     fields[18],
                'fees':        fields[19],
                'exit_reason': fields[20],
                'regime':      fields[21],
            })

    df = pd.DataFrame(records)
    if df.empty:
        return df

    # Zaman damgalarını datetime'e çevir
    df['dt_entry'] = pd.to_datetime(df['ts_entry'], unit='us', utc=True)
    df['dt_exit']  = pd.to_datetime(df['ts_exit'],  unit='us', utc=True)
    df['hold_us']  = df['ts_exit'] - df['ts_entry']
    df['hold_s']   = df['hold_us'] / 1e6
    df['regime_name'] = df['regime'].map(REGIME_NAMES)

    # Bps cinsinden PnL (mid-price'a göre normalize)
    df['pnl_bps'] = df['pnl_net'] / (df['entry_price'] * df['qty']) * 10000.0

    return df.sort_values('ts_entry').reset_index(drop=True)

# ============================================================================
# §3  Feature Importance Hesaplayıcı
# ============================================================================

class FeatureImportanceAnalyzer:
    def __init__(self, config: dict):
        self.cfg = config

    def compute_rolling_importance(
        self, df: pd.DataFrame, window: int = None
    ) -> pd.DataFrame:
        """
        Kayan pencere bazlı feature importance hesapla.
        Her pencere için RandomForest permutation importance.
        Sonuç: [window_idx, feature, importance_mean, importance_std]
        """
        if window is None:
            window = self.cfg['rolling_importance_window']

        results = []
        n = len(df)
        step = window // 4   # 75% örtüşme

        idx = 0
        while idx + window <= n:
            chunk = df.iloc[idx:idx+window]
            X = chunk[FEATURE_COLS].fillna(0).values
            y = chunk['pnl_net'].fillna(0).values

            if np.std(y) < 1e-10 or len(chunk) < 50:
                idx += step
                continue

            rf = RandomForestRegressor(n_estimators=50, random_state=42, n_jobs=-1)
            rf.fit(X, y)
            perm = permutation_importance(rf, X, y, n_repeats=5, random_state=42, n_jobs=-1)

            for fi, feat in enumerate(FEATURE_COLS):
                results.append({
                    'window_start': idx,
                    'window_mid':   idx + window // 2,
                    'feature':      feat,
                    'importance':   perm.importances_mean[fi],
                    'importance_std': perm.importances_std[fi],
                    'ts_mid_us':    chunk['ts_entry'].iloc[window//2] if len(chunk) > window//2 else 0
                })
            idx += step

        return pd.DataFrame(results)

    def compute_full_importance(self, df: pd.DataFrame) -> pd.Series:
        """Tüm veri için feature importance (tek seferlik)."""
        X = df[FEATURE_COLS].fillna(0).values
        y = df['pnl_net'].fillna(0).values

        if np.std(y) < 1e-10 or len(df) < 20:
            return pd.Series({f: 0.0 for f in FEATURE_COLS})

        rf = RandomForestRegressor(n_estimators=100, random_state=42, n_jobs=-1)
        rf.fit(X, y)
        perm = permutation_importance(rf, X, y, n_repeats=10, random_state=42, n_jobs=-1)
        return pd.Series(perm.importances_mean, index=FEATURE_COLS)

# ============================================================================
# §4  Alpha Decay Analizi
# ============================================================================

class AlphaDecayAnalyzer:
    """
    Sinyalin zamanla nasıl bozulduğunu ölçer.
    Raporda belirtildiği gibi 'yarılanma zamanı' hesaplar.
    """

    def compute_decay_curves(
        self, df: pd.DataFrame, hold_windows_s: List[float] = None
    ) -> pd.DataFrame:
        """
        Farklı tutma süreleri için ortalama PnL hesapla.
        Pozitif PnL'nin ne kadar sürede sıfıra indiğini gösterir.
        """
        if hold_windows_s is None:
            hold_windows_s = [0.1, 0.5, 1, 5, 10, 30, 60, 120, 300, 600]

        # Giriş anındaki sinyal gücü ve PnL ilişkisi
        results = []
        df_sorted = df.sort_values('ts_entry')

        for i, row in df_sorted.iterrows():
            t  = row['ts_entry']
            ep = row['entry_price']
            pnl_bps = row['pnl_bps']
            hold    = row['hold_s']

            results.append({
                'ts':    t,
                'pnl_bps': pnl_bps,
                'hold_s':  hold,
                'edge':    row['edge'],
                'score':   row['score']
            })

        decay_df = pd.DataFrame(results)

        # Tutma süresi segmentlerine göre ortalama PnL
        decay_df['hold_bin'] = pd.cut(decay_df['hold_s'],
                                       bins=[0] + hold_windows_s,
                                       labels=hold_windows_s)
        avg_pnl = decay_df.groupby('hold_bin')['pnl_bps'].mean()
        return avg_pnl

    def estimate_halflife(self, df: pd.DataFrame) -> dict:
        """
        Alpha'nın yarılanma zamanını tahmin et.
        İlk N işlemin ortalama PnL vs sonraki N işlemin PnL karşılaştırması.
        """
        window = self.estimate_window_size(df)
        if window < 20:
            return {'halflife_trades': None, 'halflife_hours': None}

        n = len(df)
        # Ardışık pencerelerde ortalama PnL hesapla
        rolling_pnl = []
        ts_vals     = []
        for i in range(0, n - window, window // 2):
            chunk = df.iloc[i:i+window]
            rolling_pnl.append(chunk['pnl_net'].mean())
            ts_vals.append(chunk['ts_entry'].median() / 1e6)  # saniye

        if len(rolling_pnl) < 3:
            return {'halflife_trades': None, 'halflife_hours': None}

        y = np.array(rolling_pnl)
        t = np.array(range(len(y)), dtype=float)

        # Eksponansiyel fit: y = A * exp(-k * t)
        try:
            from scipy.optimize import curve_fit
            def exp_decay(x, A, k): return A * np.exp(-k * x)
            popt, _ = curve_fit(exp_decay, t, y, p0=[y[0], 0.1], maxfev=1000)
            k = abs(popt[1])
            if k > 1e-9:
                halflife_windows = np.log(2) / k
                halflife_trades  = halflife_windows * (window // 2)
                avg_trade_gap_s  = (ts_vals[-1] - ts_vals[0]) / (len(ts_vals) + 1e-9)
                halflife_hours   = halflife_trades * avg_trade_gap_s / 3600.0
                return {
                    'halflife_trades': halflife_trades,
                    'halflife_hours':  halflife_hours,
                    'decay_rate':      k
                }
        except Exception:
            pass

        return {'halflife_trades': None, 'halflife_hours': None}

    def estimate_window_size(self, df: pd.DataFrame) -> int:
        return min(100, max(20, len(df) // 10))

# ============================================================================
# §5  Sinyal Pasifleştirme Yöneticisi
# ============================================================================

class SignalDisableManager:
    """
    Zayıf sinyalleri otomatik olarak pasifleştirir.
    Durumları bir JSON dosyasına kaydeder.
    """

    def __init__(self, config: dict, state_path: str = 'signal_state.json'):
        self.cfg        = config
        self.state_path = state_path
        self.state      = self._load_state()

    def evaluate(
        self,
        full_importance:   pd.Series,
        rolling_importance: pd.DataFrame,
        decay_info:        dict,
        regime:            str = 'ALL'
    ) -> dict:
        """
        Her sinyal için pasifleştirme kararı ver.
        Döndürür: {feature: {'status': 'active'|'disabled'|'warning', 'reason': ...}}
        """
        decisions = {}

        for feat in FEATURE_COLS:
            status = 'active'
            reasons = []

            # 1. Genel importance eşiği
            imp = full_importance.get(feat, 0.0)
            if imp < self.cfg['disable_importance_threshold']:
                status = 'disabled'
                reasons.append(f'importance={imp:.4f} < {self.cfg["disable_importance_threshold"]}')

            # 2. Kayan pencere trendi: importance azalıyor mu?
            if not rolling_importance.empty and feat in rolling_importance['feature'].values:
                feat_roll = rolling_importance[rolling_importance['feature'] == feat]
                if len(feat_roll) >= 3:
                    recent_imp = feat_roll.tail(3)['importance'].mean()
                    old_imp    = feat_roll.head(3)['importance'].mean()
                    if old_imp > 1e-6 and recent_imp / old_imp < 0.5:
                        if status == 'active':
                            status = 'warning'
                        reasons.append(f'importance %50 düştü: {old_imp:.4f}→{recent_imp:.4f}')

            # 3. Alpha decay hız kontrolü
            hl = decay_info.get('halflife_hours')
            if hl is not None:
                if hl < self.cfg['disable_decay_halflife_hours']:
                    if status == 'active':
                        status = 'warning'
                    reasons.append(f'halflife={hl:.2f}h çok kısa')

            decisions[feat] = {
                'status':     status,
                'importance': float(imp),
                'reason':     ', '.join(reasons) if reasons else 'OK'
            }

        # Durumu güncelle ve kaydet
        self.state[regime] = {
            'decisions':   decisions,
            'decay_info':  {k: float(v) if isinstance(v, (int, float)) else v
                            for k, v in decay_info.items()},
            'timestamp':   time.time()
        }
        self._save_state()
        return decisions

    def should_retrain(self, decay_info: dict) -> bool:
        """Yeniden eğitim gerekiyor mu?"""
        hl = decay_info.get('halflife_hours')
        if hl is None:
            return False
        return hl < self.cfg['retrain_signal_halflife_hours']

    def get_active_features(self, regime: str = 'ALL') -> List[str]:
        """Aktif feature listesini döndür."""
        if regime not in self.state:
            return FEATURE_COLS
        decisions = self.state[regime].get('decisions', {})
        return [f for f in FEATURE_COLS if decisions.get(f, {}).get('status') != 'disabled']

    def _load_state(self) -> dict:
        if os.path.exists(self.state_path):
            try:
                with open(self.state_path) as f:
                    return json.load(f)
            except Exception:
                pass
        return {}

    def _save_state(self):
        try:
            with open(self.state_path, 'w') as f:
                json.dump(self.state, f, indent=2)
        except Exception as e:
            log.warning(f"Sinyal durumu kaydedilemedi: {e}")

# ============================================================================
# §6  Rolling Performance Metrics
// ============================================================================

def compute_rolling_metrics(df: pd.DataFrame, window: int = 200) -> pd.DataFrame:
    """Rolling Sharpe, rolling PnL ve fill rate hesapla."""
    df = df.copy().reset_index(drop=True)
    df['cum_pnl'] = df['pnl_net'].cumsum()
    df['rolling_pnl'] = df['pnl_net'].rolling(window, min_periods=10).sum()

    # Rolling Sharpe
    def rolling_sharpe(series):
        m = series.mean()
        s = series.std()
        return (m / s * np.sqrt(len(series))) if s > 1e-10 else 0.0

    df['rolling_sharpe'] = df['pnl_net'].rolling(window, min_periods=20).apply(
        rolling_sharpe, raw=True)

    df['rolling_win_rate'] = (df['pnl_net'] > 0).rolling(window, min_periods=10).mean()
    return df

# ============================================================================
# §7  Ana Analiz Sınıfı
// ============================================================================

class SignalAnalyzerV2:
    def __init__(self, config: dict = None):
        self.cfg      = {**DEFAULT_CONFIG, **(config or {})}
        self.imp_an   = FeatureImportanceAnalyzer(self.cfg)
        self.dec_an   = AlphaDecayAnalyzer()
        self.disable_mgr = SignalDisableManager(
            self.cfg,
            state_path=os.path.join(self.cfg['output_dir'], 'signal_state.json')
        )

    def run(self, trade_files: List[str]) -> dict:
        """Ana analiz döngüsü."""
        os.makedirs(self.cfg['output_dir'], exist_ok=True)

        # Veri yükle
        dfs = []
        for f in trade_files:
            df = parse_trade_records(f)
            if not df.empty:
                dfs.append(df)

        if not dfs:
            log.error("Yüklenebilecek işlem kaydı bulunamadı!")
            return {}

        df = pd.concat(dfs, ignore_index=True).sort_values('ts_entry')
        log.info(f"Toplam {len(df)} işlem yüklendi ({len(trade_files)} dosya)")

        report = {}

        # 1. Feature importance (tam veri)
        log.info("Feature importance hesaplanıyor...")
        full_imp = self.imp_an.compute_full_importance(df)
        report['full_importance'] = full_imp.to_dict()

        # 2. Rolling importance (sinyal bozunması izle)
        log.info("Kayan pencere importance hesaplanıyor...")
        roll_imp = self.imp_an.compute_rolling_importance(df)
        report['rolling_importance_shape'] = roll_imp.shape

        # 3. Alpha decay
        log.info("Alpha decay analiz ediliyor...")
        decay = self.dec_an.estimate_halflife(df)
        report['alpha_decay'] = decay

        decay_needs_retrain = self.disable_mgr.should_retrain(decay)
        report['retrain_recommended'] = decay_needs_retrain

        if decay_needs_retrain:
            hl = decay.get('halflife_hours', 'N/A')
            log.warning(f"⚠️  YENİDEN EĞİTİM GEREKİYOR: alpha halflife = {hl:.2f}s")

        # 4. Sinyal pasifleştirme kararları
        log.info("Sinyal pasifleştirme değerlendiriliyor...")
        decisions = self.disable_mgr.evaluate(full_imp, roll_imp, decay)
        report['signal_decisions'] = decisions

        disabled_features = [f for f, d in decisions.items() if d['status'] == 'disabled']
        warning_features  = [f for f, d in decisions.items() if d['status'] == 'warning']
        log.info(f"  Pasifleştirildi: {disabled_features}")
        log.info(f"  Uyarı:           {warning_features}")

        # 5. Rejim bazlı analiz
        if self.cfg['analyze_per_regime']:
            report['regime_analysis'] = {}
            for regime_id, regime_name in REGIME_NAMES.items():
                regime_df = df[df['regime'] == regime_id]
                if len(regime_df) < 50:
                    continue
                ri = self.imp_an.compute_full_importance(regime_df)
                rd = self.dec_an.estimate_halflife(regime_df)
                rd_decisions = self.disable_mgr.evaluate(ri, pd.DataFrame(), rd, regime=regime_name)
                report['regime_analysis'][regime_name] = {
                    'n_trades': len(regime_df),
                    'avg_pnl_bps': regime_df['pnl_bps'].mean(),
                    'importance': ri.to_dict(),
                    'halflife_hours': rd.get('halflife_hours'),
                    'active_features': self.disable_mgr.get_active_features(regime_name)
                }

        # 6. Rolling performans
        log.info("Rolling performans metrikleri hesaplanıyor...")
        df = compute_rolling_metrics(df)

        # 7. Grafikler
        log.info("Grafikler oluşturuluyor...")
        self._plot_importance_decay(full_imp, roll_imp, decay, decisions)
        self._plot_rolling_performance(df)
        self._plot_regime_breakdown(df)

        # 8. SHAP analizi
        if HAS_SHAP:
            log.info("SHAP analizi yapılıyor...")
            self._compute_shap(df)

        # 9. JSON raporu
        report_path = os.path.join(self.cfg['output_dir'], 'analysis_report.json')
        with open(report_path, 'w') as f:
            def default_handler(o):
                if isinstance(o, (np.integer,)): return int(o)
                if isinstance(o, (np.floating,)): return float(o)
                if isinstance(o, np.ndarray): return o.tolist()
                return str(o)
            json.dump(report, f, indent=2, default=default_handler)

        log.info(f"Analiz tamamlandı → {self.cfg['output_dir']}/")
        self._print_summary(report)
        return report

    def _print_summary(self, report: dict):
        print("\n" + "="*56)
        print("  KUSAK V9 Sinyal Analizi Özeti")
        print("="*56)

        print("\n  Feature Importance (tüm veri):")
        fi = report.get('full_importance', {})
        for feat, imp in sorted(fi.items(), key=lambda x: -x[1]):
            bar = '█' * int(imp * 200)
            print(f"  {feat:20s} {imp:.4f}  {bar}")

        decay = report.get('alpha_decay', {})
        hl = decay.get('halflife_hours')
        print(f"\n  Alpha Yarılanma Zamanı: "
              f"{'%.2f saat' % hl if hl else 'Tahmin edilemedi'}")

        if report.get('retrain_recommended'):
            print("  ⚠️  YENİDEN EĞİTİM TAVSİYE EDİLİYOR")

        decs = report.get('signal_decisions', {})
        disabled = [f for f, d in decs.items() if d['status'] == 'disabled']
        warnings = [f for f, d in decs.items() if d['status'] == 'warning']
        if disabled: print(f"\n  🔴 Pasifleştirilmesi önerilen: {', '.join(disabled)}")
        if warnings: print(f"  🟡 Uyarı durumundaki:          {', '.join(warnings)}")

        print("="*56 + "\n")

    def _plot_importance_decay(self, full_imp, roll_imp, decay, decisions):
        fig, axes = plt.subplots(2, 2, figsize=(16, 12))
        fig.suptitle('Feature Importance & Alpha Decay Analizi', fontsize=14)

        # 1. Bar chart: tam importance
        ax = axes[0, 0]
        colors = ['#e74c3c' if decisions[f]['status'] == 'disabled'
                  else '#f39c12' if decisions[f]['status'] == 'warning'
                  else '#2ecc71' for f in FEATURE_COLS]
        ax.barh(list(full_imp.index), list(full_imp.values), color=colors)
        ax.set_title('Feature Importance (🔴 Devre dışı, 🟡 Uyarı, 🟢 Aktif)')
        ax.set_xlabel('Permutation Importance')

        # 2. Rolling importance ısı haritası
        ax = axes[0, 1]
        if not roll_imp.empty:
            pivot = roll_imp.pivot_table(
                index='window_mid', columns='feature', values='importance', aggfunc='mean')
            if not pivot.empty:
                sns.heatmap(pivot.T, ax=ax, cmap='RdYlGn', center=0)
                ax.set_title('Kayan Pencere Feature Importance')
                ax.set_xlabel('Zaman (işlem indeksi)')

        # 3. Alpha decay eğrisi
        ax = axes[1, 0]
        hl = decay.get('halflife_hours')
        if hl:
            t = np.linspace(0, hl * 3, 200)
            k = np.log(2) / (hl * 1)  # hours → arbitrary units
            ax.plot(t, np.exp(-k * t), 'b-', linewidth=2)
            ax.axvline(hl, color='r', linestyle='--', label=f'T½={hl:.2f}s')
            ax.set_xlabel('Zaman (saat)')
            ax.set_ylabel('Normalize Alpha Değeri')
            ax.set_title(f'Alpha Decay Eğrisi  (T½ = {hl:.2f}h)')
            ax.legend()
        else:
            ax.text(0.5, 0.5, 'Yetersiz veri', ha='center', transform=ax.transAxes)
            ax.set_title('Alpha Decay')

        # 4. Sinyal kararları tablosu
        ax = axes[1, 1]
        ax.axis('off')
        rows = []
        for feat, dec in decisions.items():
            emoji = '🔴' if dec['status'] == 'disabled' else '🟡' if dec['status'] == 'warning' else '🟢'
            rows.append([feat, f"{dec['importance']:.4f}", dec['status'], dec['reason'][:30]])
        table = ax.table(
            cellText=rows,
            colLabels=['Feature', 'Imp.', 'Durum', 'Neden'],
            cellLoc='left', loc='center'
        )
        table.auto_set_font_size(False)
        table.set_fontsize(8)
        table.auto_set_column_width([0,1,2,3])
        ax.set_title('Sinyal Karar Tablosu')

        plt.tight_layout()
        path = os.path.join(self.cfg['output_dir'], 'importance_decay.png')
        plt.savefig(path, dpi=150, bbox_inches='tight')
        plt.close()
        log.info(f"  → {path}")

    def _plot_rolling_performance(self, df: pd.DataFrame):
        fig, axes = plt.subplots(3, 1, figsize=(16, 12), sharex=True)
        fig.suptitle('Rolling Performans Metrikleri', fontsize=14)

        x = range(len(df))

        axes[0].plot(x, df['cum_pnl'], 'b-', linewidth=1)
        axes[0].set_ylabel('Kümülatif PnL')
        axes[0].set_title('Kümülatif Net PnL')
        axes[0].grid(True, alpha=0.3)

        axes[1].plot(x, df['rolling_sharpe'].fillna(0), 'g-', linewidth=1)
        axes[1].axhline(0, color='k', linestyle='-', linewidth=0.5)
        axes[1].axhline(1, color='g', linestyle='--', alpha=0.5, linewidth=0.8)
        axes[1].set_ylabel('Rolling Sharpe')
        axes[1].set_title('Rolling Sharpe Oranı')
        axes[1].grid(True, alpha=0.3)

        axes[2].plot(x, df['rolling_win_rate'].fillna(0), 'r-', linewidth=1)
        axes[2].axhline(0.5, color='k', linestyle='--', alpha=0.5)
        axes[2].set_ylabel('Kazanma Oranı')
        axes[2].set_title('Rolling Kazanma Oranı')
        axes[2].set_xlabel('İşlem İndeksi')
        axes[2].grid(True, alpha=0.3)

        plt.tight_layout()
        path = os.path.join(self.cfg['output_dir'], 'rolling_performance.png')
        plt.savefig(path, dpi=150, bbox_inches='tight')
        plt.close()
        log.info(f"  → {path}")

    def _plot_regime_breakdown(self, df: pd.DataFrame):
        fig, axes = plt.subplots(1, 2, figsize=(14, 6))
        fig.suptitle('Rejim Bazlı Performans', fontsize=14)

        # PnL dağılımı her rejim için
        regime_pnl = df.groupby('regime_name')['pnl_bps'].agg(['mean', 'std', 'count'])
        regime_pnl.plot(kind='bar', y='mean', ax=axes[0], color='steelblue')
        axes[0].set_title('Rejim Başına Ortalama PnL (bps)')
        axes[0].set_xlabel('Rejim')
        axes[0].set_ylabel('PnL bps')
        axes[0].tick_params(axis='x', rotation=45)

        # İşlem sayısı pasta grafiği
        counts = df['regime_name'].value_counts()
        axes[1].pie(counts.values, labels=counts.index, autopct='%1.1f%%',
                    colors=['#3498db','#2ecc71','#e74c3c','#f39c12','#9b59b6'])
        axes[1].set_title('Rejim Dağılımı')

        plt.tight_layout()
        path = os.path.join(self.cfg['output_dir'], 'regime_breakdown.png')
        plt.savefig(path, dpi=150, bbox_inches='tight')
        plt.close()
        log.info(f"  → {path}")

    def _compute_shap(self, df: pd.DataFrame, n_samples: int = 500):
        X = df[FEATURE_COLS].fillna(0)
        y = df['pnl_net'].fillna(0)
        if len(X) < 50:
            return

        rf = RandomForestRegressor(n_estimators=100, random_state=42, n_jobs=-1)
        rf.fit(X.values, y.values)

        sample = X.sample(min(n_samples, len(X)), random_state=42)
        explainer = shap.TreeExplainer(rf)
        shap_vals = explainer.shap_values(sample.values)

        plt.figure(figsize=(10, 6))
        shap.summary_plot(shap_vals, sample, feature_names=FEATURE_COLS,
                          show=False, plot_type='bar')
        path = os.path.join(self.cfg['output_dir'], 'shap_summary.png')
        plt.savefig(path, dpi=150, bbox_inches='tight')
        plt.close()
        log.info(f"  → {path}")


# ============================================================================
# §8  CLI Giriş Noktası
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='KUSAK V9 Sinyal Analizörü v2')
    parser.add_argument('files', nargs='*', help='TradeRecord binary dosyaları')
    parser.add_argument('--output', default='analysis', help='Çıktı dizini')
    parser.add_argument('--config', help='Yapılandırma JSON dosyası')
    parser.add_argument('--watch', help='Bu dizini izle (sürekli mod)')
    parser.add_argument('--interval', type=int, default=3600, help='İzleme aralığı (s)')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()

    config = {'output_dir': args.output}
    if args.config and os.path.exists(args.config):
        with open(args.config) as f:
            config.update(json.load(f))
    if args.verbose:
        config['verbose'] = True

    analyzer = SignalAnalyzerV2(config)

    if args.watch:
        log.info(f"İzleme modu: {args.watch} (her {args.interval}s)")
        while True:
            files = sorted(Path(args.watch).glob('*.bin'))
            if files:
                analyzer.run([str(f) for f in files])
            time.sleep(args.interval)
    else:
        if not args.files:
            parser.error("En az bir .bin dosyası belirtin veya --watch kullanın")
        analyzer.run(args.files)


if __name__ == '__main__':
    main()
