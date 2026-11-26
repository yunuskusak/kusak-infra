> ⚠️ **Bu dosya eski V10 dönemine aittir; güncel mimari ve uyarılar için [README.md](README.md) esastır.**

# KUSAK V10 — Kullanım Kılavuzu

> **Sürüm:** v4.3.0 (v10 — Rapor Tamamlama: Slippage · Kelly · Bildirim · Genetik · Paper · Benchmark · Circuit Breaker)  
> **Platform:** Linux (Ubuntu 20.04+), C++17, GCC 11+ / Clang 14+  
> **Mimari:** AVX2 destekli x86-64  
> **Yeni (v10):** `--genetic`, `--paper` modları · Buy-Hold Benchmark · Circuit Breaker · Genişletilmiş Metrikler · Stop Emir Tipleri

---

## İçindekiler

1. [Hızlı Başlangıç](#1-hızlı-başlangıç)
2. [Gereksinimler ve Derleme](#2-gereksinimler-ve-derleme)
3. [Çalışma Modları](#3-çalışma-modları)
   - [--record](#31---record-canlı-veri-kayıt-modu)
   - [--replay](#32---replay-veri-geri-oynatma)
   - [--backtest](#33---backtest-tek-backtest)
   - [--optimize](#34---optimize-parametre-optimizasyonu)
   - [--genetic](#35---genetic-genetik-evrimsel-optimizasyon-rapor-7) ⭐ YENİ
   - [--paper](#36---paper-kağıt-ticaret-modu-rapor-8) ⭐ YENİ
   - [--wfo](#37---wfo-walk-forward-optimizasyon)
   - [--regime](#38---regime-rejim-analiz-raporu)
   - [--monitor](#39---monitor-gecikme--ters-seçim-izleme)
4. [CLI Parametre Referansı](#4-cli-parametre-referansı) ⭐ GÜNCELLENDİ
5. [.env Dosyası Yapılandırması](#5-env-dosyası-yapılandırması) ⭐ GÜNCELLENDİ
6. [Slippage Modelleri](#6-slippage-modelleri-rapor-1) ⭐ YENİ
7. [Pozisyon Boyutlandırma](#7-pozisyon-boyutlandırma-rapor-2) ⭐ YENİ
8. [Stop Emir Tipleri](#8-stop-emir-tipleri-rapor-6) ⭐ YENİ
9. [Buy-Hold Benchmark ve Alfa](#9-buy-hold-benchmark-ve-alfa-rapor-4) ⭐ YENİ
10. [Genişletilmiş Backtest Metrikleri](#10-genişletilmiş-backtest-metrikleri-rapor-5) ⭐ YENİ
11. [Circuit Breaker](#11-circuit-breaker-rapor-9) ⭐ YENİ
12. [Bildirim Sistemi](#12-bildirim-sistemi-rapor-3) ⭐ YENİ
13. [Dosya Formatları](#13-dosya-formatları)
14. [CI / Sanitizer Yapılandırması](#14-ci--sanitizer-yapılandırması)
15. [Mimari ve Tasarım Kararları](#15-mimari-ve-tasarım-kararları)
16. [Test Senaryoları](#16-test-senaryoları) ⭐ GÜNCELLENDİ
17. [Sık Sorulan Sorular (SSS)](#17-sık-sorulan-sorular-sss)
18. [Hata Referansı](#18-hata-referansı)

---

## 1. Hızlı Başlangıç

```bash
# 0. Bağımlılık kur (OpenSSL — WebSocket TLS için zorunlu)
sudo apt-get install -y libssl-dev   # Ubuntu/Debian
# sudo yum install -y openssl-devel  # RHEL/CentOS

# 1. Derle (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. .env oluştur ve doldur
cp .env.example .env
# .env dosyasını düzenle: API_KEY, API_SECRET, SYMBOL
# Telegram/Slack bildirimleri için: TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID

# 3. Kayıt başlat (Binance WS → .ticks dosyası)
./build/research --record --output data
# Ctrl+C ile durdur

# 4. Temel analiz
./build/research --replay   data/ticks/BTCUSDT_20240101_120000.ticks
./build/research --backtest data/ticks/BTCUSDT_20240101_120000.ticks
./build/research --regime   data/ticks/BTCUSDT_20240101_120000.ticks

# ── YENİ v10 iş akışı ────────────────────────────────────────────────────

# 5. Genetik optimizasyon (50 bireylik popülasyon, 30 nesil)
./build/research --genetic data/ticks/BTCUSDT.ticks \
    --population 50 --generations 30 --workers 8 --output data

# 6. Paper Trading (gerçek para riske atmadan canlı test)
./build/research --paper data/ticks/BTCUSDT.ticks \
    --buy-pct 0.50 --kelly 0.25 --cb-drawdown 0.15

# 7. Gerçekçi slippage ile backtest (Volume Share modeli)
./build/research --backtest data/ticks/BTCUSDT.ticks \
    --slippage volume

# 8. Circuit Breaker aktif, Kelly ile optimize edilmiş backtest
./build/research --backtest data/ticks/BTCUSDT.ticks \
    --slippage volatility --kelly 0.30 --cb-drawdown 0.20
```

---

## 2. Gereksinimler ve Derleme

### Sistem Gereksinimleri

| Bileşen | Minimum | Önerilen |
|---------|---------|----------|
| İşletim Sistemi | Linux (Ubuntu 20.04+) | Ubuntu 22.04 / Debian 12 |
| Derleyici | GCC 11 veya Clang 14 | GCC 13 / Clang 17 |
| CMake | 3.17+ | 3.25+ |
| CPU | x86-64 | AVX2 destekli (Intel Haswell+, AMD Zen+) |
| RAM | 2 GB | 8 GB+ (büyük tick dosyaları için) |

### Bağımlılıklar

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential cmake libpthread-stubs0-dev
```

### Derleme Seçenekleri

**Release (üretim):**
```bash
cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_AVX2=ON
cmake --build build -j$(nproc)
```

**Debug + Sanitizer (geliştirme):**
```bash
cmake -S . -B build-debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_SANITIZERS=ON \
      -DENABLE_AVX2=OFF
cmake --build build-debug -j$(nproc)
```

**AVX2 olmadan (CI ortamı):**
```bash
cmake -S . -B build-ci \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_AVX2=OFF
cmake --build build-ci -j$(nproc)
```

**Thread Sanitizer (yarış koşulu tespiti):**
```bash
cmake -S . -B build-tsan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TSAN=ON \
      -DENABLE_SANITIZERS=OFF \
      -DENABLE_AVX2=OFF
cmake --build build-tsan -j$(nproc)
```

### Etkin Derleme Uyarıları

`CMakeLists.txt` aşağıdaki uyarı bayraklarını içerir:

```cmake
-Wall -Wextra -Wpedantic
-Wpsabi        # psABI alignment değişikliklerini yakalamak için açık kalmalı
-Wabi=11       # GCC ABI baseline 11 — boş -Wabi yerine belirli sürüm kullanın
-Wno-unused-parameter
```

> **Not:** `-Wabi` (sürümsüz) modern GCC'de hiçbir şey uyarmaz. `-Wabi=11` ile
> pinleyerek deterministik bir kalite sinyali elde edilir.

---

## 3. Çalışma Modları

### Genel Kullanım Sözdizimi

```
./build/research <mod> [dosya] [seçenekler]
```

---

### 3.1 `--record` — Canlı Veri Kayıt Modu

Piyasadan gelen canlı tick verisini binary `.ticks` dosyasına yazar.

```bash
./build/research --record [--output <dizin>]
```

**Ön Koşullar:**
- Proje kökünde (veya üst dizinlerde) `.env` dosyası olmalıdır.
- `.env` içinde `API_KEY` ve `API_SECRET` tanımlı olmalıdır.

**Örnek:**
```bash
./build/research --record --output /data/ticks
```

**Çıktı:**
```
[record] Canlı kayıt modu baslatiliyor...
[record] .env bulundu: /home/user/project/.env
[record] .env yuklendi: API_KEY=abc...xyz  API_SECRET=def...uvw
[record] Recorder basladi.
  Output : /data/ticks/ticks
  Symbol : BTCUSDT
  (Durdurmak icin Ctrl+C)
[record][stats]  msgs=0  bytes=0 (0.00 MiB)  files=1  drops=0  rate=0 msg/s  0.00 MiB/s
```

**Akış Diyagramı:**
```
CLI: --record
    │
    ▼
.env keşfi (cwd + 5 üst dizin)
    │ bulunamadı → stderr + exit 1
    ▼
.env ayrıştırma (export, tırnak, #yorum, trim)
    │ API_KEY/SECRET eksik → stderr + exit 1
    ▼
output/ticks dizini oluştur + yazma erişimi test
    │ başarısız → stderr + exit 1
    ▼
MarketDataRecorder::start()
    │
    ▼
Ana döngü (Ctrl+C'ye kadar, her 1s stats)
    │ Ctrl+C
    ▼
recorder.stop() + temiz çıkış (exit 0)
```

> **Önemli:** WebSocket/REST canlı feed entegrasyonu bu sürümde belirsizdir (UNSPECIFIED).
> Recorder başlatılır ve dosya başlığı yazılır; gerçek bir producer olmadan `.ticks`
> dosyasında yalnızca header bulunur.

---

### 3.2 `--replay` — Veri Geri Oynatma

Kaydedilmiş `.ticks` dosyasını RegimeDetectorV2, VPIN ve isteğe bağlı gecikme
izleme ile işler.

```bash
./build/research --replay <dosya.ticks> [--speed <N>] [--monitor]
```

| Seçenek | Varsayılan | Açıklama |
|---------|-----------|----------|
| `--speed <N>` | 100 | Replay hız çarpanı (100x = gerçek zamandan 100 kat hızlı) |
| `--monitor` | kapalı | Gecikme P99 ve ters seçim izlemeyi aktifleştirir |

**Örnekler:**
```bash
# Varsayılan 100x hızda replay
./build/research --replay data/ticks/BTCUSDT_20240101.ticks

# 1x hız (gerçek zamanlama) + monitor
./build/research --replay data/ticks/BTCUSDT_20240101.ticks --speed 1 --monitor

# Mümkün olan en hızlı hız
./build/research --replay data/ticks/BTCUSDT_20240101.ticks --speed 999999
```

**Çıktı:** Her 1000 tick'te bir rejim değişikliği konsola yazılır. `--monitor` aktifse
gecikme P50/P95/P99 ve ters seçim raporu eklenir.

---

### 3.3 `--backtest` — Tek Backtest

Varsayılan parametrelerle tek bir backtest çalıştırır.

```bash
./build/research --backtest <dosya.ticks>
```

**Örnek Çıktı:**
```
[Backtest] Sonuçlar:
  Süre          : 145.23 ms
  İşlem sayısı  : 1247
  Net PnL       : 0.0523
  Sharpe        : 1.3412
  Sortino       : 1.8901
  Max DD        : 0.0087
  Kazanma oranı : 54.37 %
  Fill oranı    : 72.11 %
  Objektif      : 0.6234
```

> **Not:** Tick sayısı 10'dan az ise "GEÇERSİZ" mesajı görünür.

---

### 3.4 `--optimize` — Parametre Optimizasyonu

Grid arama + Bayesian optimizasyon ile en iyi parametreleri bulur.

```bash
./build/research --optimize <dosya.ticks> \
    [--workers <N>] \
    [--bayes <N>] \
    [--output <dizin>]
```

| Seçenek | Varsayılan | Açıklama |
|---------|-----------|----------|
| `--workers <N>` | CPU çekirdek sayısı | Paralel iş parçacığı sayısı |
| `--bayes <N>` | 50 | Bayesian iterasyon sayısı |
| `--output <D>` | `data/` | Sonuç çıktı dizini |

**Örnek:**
```bash
./build/research --optimize data/ticks/BTCUSDT.ticks \
    --workers 8 \
    --bayes 100 \
    --output results/
```

**Çıktılar:**
- `results/best_params.txt` — En iyi parametreler
- `results/opt_results.csv` — Tüm iterasyon sonuçları

**Optimize Edilen Parametreler:**

| Parametre | Aralık | Açıklama |
|-----------|--------|----------|
| `vpin_threshold` | 0.3 – 0.95 | VPIN alarm eşiği |
| `min_edge` | 0.0001 – 0.002 | Minimum kenar marjı |
| `imbalance_threshold` | 0.1 – 0.6 | Emir defteri dengesizlik eşiği |
| `fill_prob_threshold` | 0.005 – 0.05 | Doldurulma olasılığı filtresi |
| `sniper_score_mult` | 1.0 – 3.0 | Sniper skoru çarpanı |
| `max_depth` | 500 – 5000 | Maksimum kuyruk derinliği |
| `trailing_activation_bps` | 5 – 30 | Trailing stop aktivasyon eşiği (bps) |
| `trailing_callback_bps` | 2 – 15 | Trailing stop geri çekilme eşiği (bps) |

---

### 3.5 `--genetic` — Genetik/Evrimsel Optimizasyon ⭐ YENİ (Rapor §7)

Parametre uzayını evrimsel algoritma ile tarar. Grid veya Bayes'in atlayabileceği bölgeleri bulur.

```bash
./build/research --genetic <dosya.ticks> \
    [--population <N>] \
    [--generations <N>] \
    [--workers <N>] \
    [--output <dizin>]
```

| Seçenek | Varsayılan | Açıklama |
|---------|-----------|----------|
| `--population <N>` | 50 | Nesil başına birey sayısı |
| `--generations <N>` | 30 | Maksimum nesil sayısı |
| `--workers <N>` | CPU çekirdeği | Paralel fitness hesaplama iş parçacığı sayısı |

**Algoritma Adımları:**
1. `--population` büyüklüğünde rastgele parametre seti (birey) üretilir
2. Her birey için backtest çalıştırılır → fitness hesaplanır (Sharpe ağırlıklı)
3. **Tournament seçimi** ile en iyi bireyler seçilir
4. **Uniform crossover** ile yeni bireyler üretilir
5. **Gaussian mutasyon** ile çeşitlilik korunur
6. **Elitizm**: En iyi 3 birey değişmeden sonraki nesle aktarılır
7. N nesil veya 8 nesil iyileşme yoksa durulur

**Örnek Çıktı:**
```
[GeneticOpt] Başlatılıyor
  Popülasyon : 50
  Max Nesil  : 30
  İş parçacığı: 8
  Çaprazlama : 0.70
  Mutasyon   : 0.15

Nesil  En İyi Fit.  Ort. Fit.   Süre (s)
--------------------------------------------------
0      0.004231     0.001823    12.3
1      0.005118     0.002341    24.1
...
15     0.007823     0.005234    185.2
[GeneticOpt] 8 nesil iyileşme yok — duruldu.

[GeneticOpt] === En İyi Birey ===
  Fitness              : 0.007823
  vpin_threshold       : 0.638
  imbalance_threshold  : 0.287
  trailing_activation  : 12.4 bps
  trailing_callback    : 6.1 bps
```

**Çıktı Dosyası:** `<output>/genetic_best_params.txt`

**Tavsiye:** Önce `--optimize` (Grid + Bayes) ile dar bir bölge bulun, ardından `--genetic` ile ince arama yapın.

---

### 3.6 `--paper` — Kağıt Ticaret Modu ⭐ YENİ (Rapor §8)

Gerçek para riske atmadan, canlı tick stream üzerinde strateji simülasyonu.  
**Gerçek borsaya hiçbir emir gönderilmez.** İşlem ücretleri ve slippage gerçekçi hesaplanır.

```bash
./build/research --paper <dosya.ticks> \
    [--buy-pct <0.0-1.0>] \
    [--kelly <0.0-1.0>] \
    [--cb-drawdown <0.0-1.0>] \
    [--slippage <model>]
```

| Seçenek | Varsayılan | Açıklama |
|---------|-----------|----------|
| `--buy-pct <f>` | 0 (sabit lot) | İşlem başına sermayenin bu yüzdesi kullanılır |
| `--kelly <f>` | 0 (kapalı) | Kelly fraksiyonu (geçmiş işlemlerden otomatik hesaplanır) |
| `--cb-drawdown <f>` | 0.20 | Bu drawdown yüzdesinde circuit breaker devreye girer |
| `--slippage <model>` | ticks | Slippage modeli (bkz. §6) |

**Örnek — %50 sermaye ile paper trading:**
```bash
./build/research --paper data/ticks/BTCUSDT.ticks \
    --buy-pct 0.50 \
    --cb-drawdown 0.15 \
    --slippage volume
```

**Periyodik Özet (her 10.000 tick):**
```
╔══ PAPER TRADING ÖZET ══════════════════════════
║ Sembol     : BTCUSDT
║ Tick sayısı: 50000
║ İşlem sayısı: 47
║ Kümül PnL  : +12.3482 USDT
║ Sermaye    : 1012.3482 USDT
║ Win Rate   : 61.7%
║ Max DD     : 4.2100 USDT
║ Süre       : 23.4 s
╚═══════════════════════════════════════════════
```

**Telegram Bildirimleri:**  
`.env` içinde `TELEGRAM_BOT_TOKEN` ve `TELEGRAM_CHAT_ID` tanımlıysa, her işlem açılışı/kapanışı ve circuit breaker olayı Telegram'a gönderilir.

**Backtest ile Karşılaştırma:**  
Paper trading sonunda tam `BacktestResult` raporu üretilir (Buy-Hold Benchmark, Profit Factor dahil). Sonuçların backtest ile yaklaşık tutarlı olması beklenir.

---

### 3.7 `--wfo` — Walk-Forward Optimizasyon

Overfitting'i önlemek için kayan pencereli optimizasyon.

```bash
./build/research --wfo <dosya.ticks> \
    [--wfo-train <N>] \
    [--wfo-test <N>] \
    [--wfo-step <N>] \
    [--workers <N>] \
    [--bayes <N>] \
    [--output <dizin>]
```

| Seçenek | Varsayılan | Açıklama |
|---------|-----------|----------|
| `--wfo-train <N>` | 100000 | Eğitim penceresi (tick sayısı) |
| `--wfo-test <N>` | 25000 | Test penceresi (tick sayısı) |
| `--wfo-step <N>` | 25000 | Kaydırma adımı (tick sayısı) |

**Gereksinim:** `--wfo-train + --wfo-test` kadar tick verisi gereklidir.

**Çıktılar:**
- `<output>/wfo_report.csv` — Her pencere için IS/OOS sonuçları
- `<output>/wfo_best_params.txt` — Tavsiye edilen parametreler (deploy edilebilir ise)

**Deploy Kararı:** WF oranı ≥ 0.60 ve OOS Sharpe ≥ 0.5 ise parametre seti önerilir.

---

### 3.6 `--regime` — Rejim Analiz Raporu

RegimeDetectorV2 ile piyasa rejimi sınıflandırması yapar.

```bash
./build/research --regime <dosya.ticks>
```

**Örnek Çıktı:**
```
[Rejim] Sınıflandırma Raporu (500000 tick):

  UNKNOWN    1234 tick   0.2%  #
  TREND     98765 tick  19.8%  ##########
  RANGE    234567 tick  46.9%  #######################
  TOXIC     89012 tick  17.8%  #########
  DEAD      76422 tick  15.3%  ########

  Rejim geçiş sayısı: 847
  İlk 10 geçiş:
    ts=1704067200000000  → RANGE
    ts=1704067215000000  → TREND
    ...
```

**Piyasa Rejimleri:**

| Rejim | Açıklama |
|-------|----------|
| `TREND` | Yönlü momentum, düşük VPIN, genişleyen ATR |
| `RANGE` | Salınımlı hareket, dar spread, dengeli emir akışı |
| `TOXIC` | Yüksek VPIN, geniş spread, bilgili emir akışı |
| `DEAD` | Düşük hacim, çok dar spread, yön yok |

---

### 3.7 `--monitor` — Gecikme + Ters Seçim İzleme

Gecikme P99 ve ters seçim ölçümü ile tam izleme raporu üretir.

```bash
./build/research --monitor <dosya.ticks> [--output <dizin>]
```

**Çıktılar:**
- `<output>/latency_monitor.csv` — Gecikme histogramı ve P50/P95/P99
- `<output>/adverse_selection.csv` — Ters seçim analizi

**Simülasyon Notu:** `--monitor` modu her 200 tick'te bir pasif alış emri
doldurulduğunu simüle eder. Gerçek fill veriniz yoksa bu modu referans olarak kullanın.

---

## 13. .env Gelişmiş Sözdizimi

### .env Gelişmiş Sözdizimi

`.env` dosyası proje kökünde veya üst dizinlerde aranır (maksimum 5 seviye yukarı).

### Desteklenen Sözdizimi

```bash
# Düz atama
API_KEY=abc123def456

# Çift tırnak (boşluk içerebilir)
API_KEY="abc 123"

# Tek tırnak
API_SECRET='def456ghi789'

# export öneki (bash uyumlu)
export SYMBOL=ETHUSDT

# Satır içi yorum (tırnaksız değerlerde)
SYMBOL=BTCUSDT  # varsayılan sembol

# Windows CRLF toleranslı
```

### Alanlar

| Alan | Zorunlu | Varsayılan | Açıklama |
|------|---------|-----------|----------|
| `API_KEY` | ✅ Evet | — | Exchange API anahtarı |
| `API_SECRET` | ✅ Evet | — | Exchange API sırrı |
| `SYMBOL` | Hayır | `BTCUSDT` | İzlenecek sembol |

### Güvenlik Uyarıları

- `.env` dosyasını asla Git'e commit etmeyin.
- `.gitignore`'a `.env` ekleyin: `echo ".env" >> .gitignore`
- API anahtarlarınıza yalnızca **okuma** yetkisi verin (canlı kayıt için yazma gerekmez).

---

## 14. Dosya Formatları

### .ticks Dosya Formatı (v0x0004)

```
[FileHeader — 72 bayt]
[TickRecordDisk — 719 bayt] × N
```

**FileHeader (72 bayt, packed):**

| Offset | Boyut | Alan | Açıklama |
|--------|-------|------|----------|
| 0 | 4 | `magic` | `0x4B55534B` ("KUSK") |
| 4 | 2 | `version` | `0x0004` |
| 6 | 2 | `flags` | Ayrılmış |
| 8 | 8 | `ts_created` | Dosya oluşturma zamanı (μs) |
| 16 | 8 | `record_count` | Kayıt sayısı (kapanışta güncellenir) |
| 24 | 32 | `symbol` | Sembol adı (null-terminated) |
| 56 | 16 | `reserved` | Gelecek kullanım |

**TickRecordDisk (719 bayt, packed):**
- Bellekteki `TickRecord` (768 bayt, `alignas(64)`) ile aynı içerik, hizalama dolgusu olmadan.
- Disk okuma/yazma `memcpy` ile yapılır — `reinterpret_cast` kullanılmaz, UB riski yoktur.

**Dosya isimlendirme:**
```
<sembol>_<YYYYMMDD>_<HHMMSS>.ticks
Örnek: BTCUSDT_20240101_120000.ticks
```

### Dosya Rotasyonu

Varsayılan maksimum dosya boyutu **1 GB**. Bu sınıra ulaşıldığında:
1. Mevcut dosya güvenli şekilde kapatılır (`fdatasync`).
2. Yeni bir zaman damgalı dosya açılır.
3. Kayıt kesintisiz devam eder.

---

## 15. CI / Sanitizer Yapılandırması

### Önerilen CI Pipeline

```yaml
# .github/workflows/ci.yml örneği
jobs:
  build-release:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: |
          cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DENABLE_AVX2=OFF
          cmake --build build -j$(nproc)

  build-sanitizers:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: |
          cmake -S . -B build-debug \
            -DCMAKE_BUILD_TYPE=Debug \
            -DENABLE_SANITIZERS=ON \
            -DENABLE_AVX2=OFF
          cmake --build build-debug -j$(nproc)
```

### Aktif Sanitizer'lar (Debug)

| Sanitizer | Bayrak | Yakaladığı |
|-----------|--------|------------|
| ASan | `-fsanitize=address` | Bellek taşmaları, use-after-free |
| UBSan | `-fsanitize=undefined` | Tanımsız davranış |
| AlignSan | `-fsanitize=alignment` | Hizalanmamış pointer erişimi |

> **Uyarı:** ASan ve TSan aynı anda kullanılamaz. Thread Sanitizer için
> `-DENABLE_TSAN=ON -DENABLE_SANITIZERS=OFF` kullanın.

### Uyarı Bayrakları Politikası

```cmake
# Aktif (kalsın):
-Wall -Wextra -Wpedantic
-Wpsabi     # psABI sinyali — over-aligned değer tipler için uyarır
-Wabi=11    # GCC ABI baseline 11 (boş -Wabi modern GCC'de hiçbir şey yapmaz)
-Wcomment   # Backslash-newline içeren // yorumları yakalar (-Werror ile build kırar)

# Opsiyonel (yalnızca toolchain hermetikse):
# -Wno-psabi  # Gürültü bastırma — tek GCC sürümü/libstdc++ ortamında düşünün
```

---

## 16. Mimari ve Tasarım Kararları

### Disk vs Bellek Tip Ayrımı

```
Yazar (WS IO iş parçacığı):
  TickRecord (768B, alignas(64)) → to_disk() → TickRecordDisk (719B, packed)
  TickRecordDisk → memcpy → mmap dosyası

Okuyucu (replay/backtester):
  mmap dosyası → memcpy → TickRecordDisk (local)
  TickRecordDisk → from_disk() → TickRecord (aligned, bellekte)
```

Bu tasarım iki şeyi sağlar:
1. **Disk alanı tasarrufu:** `alignas(64)` dolgusu (49 bayt) diske yazılmaz → %6.25 tasarruf
2. **UB yok:** `reinterpret_cast` kullanılmaz; `memcpy` hizalama gerektirmez

### SPSC Ring Buffer

Recorder, WS IO iş parçacığı ile yazar iş parçacığı arasında kilitlenmez SPSC kuyruğu kullanır:
- Kapasite: 65536 kayıt
- `push()` gecikmesi: ~50 ns
- `write()` gecikmesi: ~20 ns (mmap — steady state'te syscall yok)

### Crash-Safe Yazma

1. Her 10 saniyede bir `fdatasync` çağrılır (§5.1).
2. Dosya kapatılmadan önce `ftruncate` ile tam boyuta ayarlanır.
3. `record_count` başlığa `memcpy` ile her 1024 kayıtta bir güncellenir.

### `alignas(64)` Kullanım Politikası

| Tip | `alignas(64)` | Gerekçe |
|-----|---------------|---------|
| `TickRecord` | ✅ Var | Hot-path IO, AVX2 vectorization, false sharing önleme |
| `TradeRecord` | ✅ Var | Cache line boundary hizalaması |
| `ResearchParams` | ❌ Kaldırıldı | STL container value type — `-Wpsabi` uyarısı tetikliyordu |
| `RecorderQueue` | ✅ Var (class) | False sharing önleme (producer/consumer cache lines) |

---

## 17. Test Senaryoları

Kod değişikliklerinden önce ve sonra aşağıdaki testleri çalıştırın:

### Test 1: .env Yok

```bash
mv .env .env.bak
./build/research --record
# Beklenen: hata mesajı + exit code 1
echo "Exit code: $?"  # 1 olmalı
mv .env.bak .env
```

### Test 2: Eksik API Anahtarı

```bash
echo "API_KEY=" > /tmp/test.env
echo "API_SECRET=" >> /tmp/test.env
# .env'yi geçici olarak /tmp/test.env ile simüle edin
# Beklenen: "HATA: API_KEY / API_SECRET eksik" + exit 1
```

### Test 3: Tırnaklı Değer

```bash
echo 'API_KEY="  değer ile boşluk  "' > .env
echo 'API_SECRET=gizli # bu yorum olmamalı' >> .env
./build/research --record --output /tmp/test_out
# Beklenen: trim+unquote doğru çalışmalı
```

### Test 4: Yazılamaz Çıktı Dizini

```bash
./build/research --record --output /root/forbidden
# Beklenen: "cikti dizinine yazilamiyor" + exit 1
```

### Test 5: Ctrl+C Temiz Kapanış

```bash
./build/research --record --output /tmp/record_test &
PID=$!
sleep 3
kill -INT $PID
wait $PID
echo "Exit code: $?"  # 0 olmalı
```

### Test 6: Replay Tutarlılığı

```bash
# Önce replay ile backtest
./build/research --backtest data/ticks/test.ticks
# Sonra aynı dosyayı iki kez çalıştır — sonuçlar özdeş olmalı (deterministik)
```

---

### ⭐ YENİ v10 Test Senaryoları

---

### Test 7: Win Streak ve Profit Factor — Birim Doğrulama (Rapor §5)

Küçük, bilinen trade listesiyle metriklerin doğru hesaplandığını kontrol edin.

**Beklenen değerler:**

```
Trades: [+10, +5, -2, +8, -3, +4, +6, -1]
  Kazanç işlemleri: 5 (+10, +5, +8, +4, +6) → Toplam = 33
  Kayıp işlemleri : 3 (-2, -3, -1)            → Toplam = 6
  Profit Factor   = 33 / 6 = 5.50
  Max Win Streak  = 3  (+8, +4, +6 ardışık)
  Max Loss Streak = 1
  Avg Win         = 33 / 5 = 6.60
  Avg Loss        = 6  / 3 = 2.00
  Win Rate        = 5  / 8 = 62.5%
```

**Test komutu:**

```bash
# Deterministik parametrelerle çalıştır
./build/research --backtest tests/fixtures/known_trades.ticks

# Raporda bu değerleri kontrol et:
# Profit Factor: 5.50 (veya yakın)
# Maks Kazanç Serisi: 3
# Ort. Kazanan İşlem: ~6.60
```

---

### Test 8: Buy-Hold Benchmark ve Alfa (Rapor §4)

**Senaryo:**
```
Başlangıç fiyatı  : 100.00 USDT  (ilk tick)
Bitiş fiyatı      : 120.00 USDT  (son tick)
Başlangıç sermayesi: 1000.00 USDT

Buy-Hold hesabı:
  qty   = 1000 / 100 = 10 BTC
  getiri = (120 - 100) × 10 = 200 USDT (20%)

Strateji net PnL: 250 USDT (25%)
Alfa = 250 - 200 = +50 USDT → "✅ Piyasayı yendi!"
```

**Kontrol edilecek çıktı:**
```
Buy&Hold Getirisi: +200.0000 USDT (20.00%)
Strateji Getirisi: 250.0000 USDT
ALFA (α)         : +50.0000  ✅ Piyasayı yendi!
```

---

### Test 9: Stop-Loss Tetikleme (Rapor §6)

**Senaryo: Long pozisyon, stop=99,500, fiyat 100,000'den 99,400'e iner**

```bash
./build/research --backtest tests/fixtures/sl_trigger.ticks --slippage ticks
```

**Beklenen davranış:**
```
Fiyat akışı: 100000 → 100200 → 99800 → 99400
                                         ↑ Burada stop-loss tetiklenmeli

Backtest çıktısında:
  [close] sebep: "sl"  fiyat ≈ 99500
  sl işlem sayısı: 1
```

**Test başarı kriteri:** Pozisyon 99,400'de değil, **99,500 civarında** kapanmalı.

---

### Test 10: Circuit Breaker — Drawdown Aşımı (Rapor §9)

**%5 eşikle çalıştır, stratejinin kaybetmesini bekle:**

```bash
./build/research --paper tests/fixtures/volatile.ticks \
    --cb-drawdown 0.05 \
    --buy-pct 0.8
```

**Beklenen çıktı:**
```
[PaperTrader] ⚠️ CIRCUIT BREAKER: Max drawdown aşıldı! DD=%5.XX
[PaperTrader] Tüm işlemler durduruldu.

Backtest Raporunda:
  Toplam İşlem: N  (circuit breaker tetiklenmeden önceki işlemler)
  Circuit Breaker: Aktif
```

**Doğrulama:** Circuit breaker tetiklendikten sonra `orders_this_session_` artmamalı.

---

### Test 11: Slippage Model Karşılaştırması (Rapor §1)

Aynı tick dosyasıyla 3 farklı slippage modelini çalıştırıp net PnL farkını gözlemleyin.

```bash
./build/research --backtest data.ticks --slippage ticks      > /tmp/sl_ticks.txt
./build/research --backtest data.ticks --slippage volume     > /tmp/sl_volume.txt
./build/research --backtest data.ticks --slippage volatility > /tmp/sl_vol.txt

grep "Net PnL\|Toplam Slippage" /tmp/sl_*.txt
```

**Beklenen sıralama:**
```
ticks modeli      → en düşük slippage maliyeti (gerçekçi değil)
volume modeli     → orta slippage (hacim bazlı gerçekçi)
volatility modeli → en yüksek slippage (en tutucu tahmin)
```

Net PnL: `ticks > volume > volatility` (slippage maliyeti yükseldikçe net PnL düşer)

---

### Test 12: Genetik Optimizasyon Yakınsama Testi (Rapor §7)

```bash
./build/research --genetic data.ticks \
    --population 30 --generations 20 --workers 4 \
    --output /tmp/genetic_test

cat /tmp/genetic_test/genetic_best_params.txt
```

**Başarı kriterleri:**
1. En son neslin fitness değeri ilk nesle göre **≥ %10 artmış** olmalı
2. `genetic_best_params.txt` oluşmuş ve parse edilebilir olmalı
3. Stall detection çalışmalı: 8 nesil iyileşme yoksa "duruldu" mesajı görünmeli

---

### Test 13: Paper Trading — Backtest Tutarlılığı (Rapor §8)

Paper trading sonucu ile aynı parametreli backtest sonucu yaklaşık tutarlı olmalıdır.

```bash
# Backtest
./build/research --backtest data.ticks --buy-pct 0.3 --slippage volume \
    > /tmp/bt_result.txt

# Paper trading (aynı veriyle)
./build/research --paper data.ticks --buy-pct 0.3 \
    > /tmp/paper_result.txt

# Karşılaştır
grep "Win Rate\|Net PnL\|Profit Factor" /tmp/bt_result.txt /tmp/paper_result.txt
```

**Kabul kriteri:** Win rate ve net PnL arasında **±5%** sapma normal sayılır.  
Daha büyük sapmalar slippage/komisyon farkından kaynaklanıyorsa beklenen davranıştır.

---

## 18. Sık Sorulan Sorular (SSS)

**S: `.ticks` dosyasında yalnızca header var, veri yok?**  
C: Bu normal davranıştır. WebSocket producer entegrasyonu bu sürümde
tamamlanmamıştır (UNSPECIFIED). Recorder başlatılır ve dosya açılır; ancak
gerçek bir WS bağlantısı olmadan tick gelmez.

**S: `-Wabi=11` uyarısı görüyorum, ne yapmalıyım?**  
C: Hiçbir şey yapmanıza gerek yok. `CMakeLists.txt` zaten `-Wabi=11` ile
pinlenmiş durumda. Bu bayrak ABI baseline 11'den sapmaları raporlar.

**S: `-Wpsabi` notları "64-byte alignment ABI changed" diyor, tehlikeli mi?**  
C: `ResearchParams`'tan `alignas(64)` kaldırıldı (R5 düzeltmesi). Diğer
`alignas(64)` tipleri (`TickRecord`, `TradeRecord`) hot-path IO yapıları
olduğundan ve public API'de by-value geçilmediğinden bu notlar zararsızdır.

**S: Sanitizer ile derleme yavaşladı, normal mi?**  
C: Evet. ASan tipik olarak 2x yavaşlatır, UBSan daha azdır. Debug derlemesi
yalnızca geliştirme sürecinde kullanılır, üretimde Release kullanın.

**S: Birden fazla sembol aynı anda kaydedilebilir mi?**  
C: Bu sürümde tek sembol desteklenmektedir. Birden fazla sembol için birden
fazla process çalıştırabilir ve farklı `--output` dizinleri belirtebilirsiniz.

**S: `.ticks` dosyası bozulursa ne olur?**  
C: `TickDataLoader` ve `ReplayEngine` truncated/bozuk dosyaları güvenli şekilde
işler: `record_count` başlık değeri aşılıyorsa gerçek dosya boyutuna göre
hesaplama yapar, eksik/kısmi son kaydı atlar (SIGBUS koruması §3.1).

**S: `TickRecord` boyutu değişirse ne olur?**  
C: `static_assert(sizeof(TickRecord) == 768)` derlemeyi kırar. Ayrıca
`TICK_FILE_VERSION` sabitini artırmanız ve `replay_engine.hpp` ile
`backtester.hpp`'teki sürüm kontrolünü güncellemeniz gerekir.

---

## 19. Hata Referansı

| Hata Mesajı | Neden | Çözüm |
|-------------|-------|-------|
| `.env bulunamadi` | .env dosyası mevcut değil | Proje kökünde `.env` oluşturun |
| `.env acilamadi` | Okuma izni yok | `chmod 600 .env` |
| `API_KEY / API_SECRET eksik` | .env'de bu anahtarlar tanımlı değil | .env'e `API_KEY=...` ve `API_SECRET=...` ekleyin |
| `cikti dizinine yazilamiyor` | Dizin izni yok | `chmod 755 <dizin>` veya farklı `--output` kullanın |
| `cikti dizini hazirlanamadi` | Dosya sistemi dolu veya hata | `df -h` ile disk doluluk kontrolü |
| `bad magic in` | .ticks dosyası geçersiz | Dosyanın gerçekten KUSAK formatında olduğunu doğrulayın |
| `version mismatch` | Eski format dosyası | v0x0004 formatında yeniden kaydedilmiş dosya kullanın |
| `file too small` | Truncated dosya | Kısmi veri: dosya yazılırken process crash olmuş olabilir |
| `GEÇERSİZ — yetersiz işlem` | < 10 işlem | Daha uzun/hareketli bir tick dosyası kullanın |
| `Yeterli tick yok` | --wfo için yetersiz veri | train+test pencerelerini küçültün veya daha büyük dosya kullanın |

---

## Lisans ve İletişim

Bu kılavuz KUSAK V10 (v4.3.0) production-ready sürümü için hazırlanmıştır.

**Değişiklik Özeti (v10 — bu sürüm):**
- ✅ §1 Slippage Modelleri: 4 model (FixedTicks, FixedBasisPoints, VolumeShare, VolatilityVolume)
- ✅ §2 Pozisyon Boyutlandırma: Kelly kriteri + buy_pct + max_position_pct
- ✅ §3 Bildirim Sistemi: Telegram, Slack, webhook — asenkron worker thread
- ✅ §4 Buy-Hold Benchmark: alpha hesaplama, buy_hold_return karşılaştırması
- ✅ §5 Genişletilmiş Metrikler: profit_factor, streak, avg win/loss, exposure %, round-trip
- ✅ §6 Stop Emir Tipleri: StopOrderType enum, STOP_LOSS, STOP_LIMIT, TRAILING_STOP
- ✅ §7 Genetik Optimizasyon: tournament seçimi, uniform crossover, Gaussian mutasyon
- ✅ §8 Paper Trading: --paper modu, gerçekçi slippage + bildirim entegrasyonu
- ✅ §9 Circuit Breaker: max_drawdown_stop_pct, max_orders_per_session, auto-close

**Önceki Düzeltmeler:**
- R7: `--record` modu Binance WebSocket stream'e tam entegrasyon
- R6: production-critical kod yollarında `reinterpret_cast` kaldırıldı
- R5: `ResearchParams`'tan `alignas(64)` kaldırıldı (`-Wpsabi` fix)
- R4: `-Wabi=11` baseline pin
- R3: `-Wcomment` fix
- R2: `.env` doğrulama ve API key maskeleme
- R1: `--record` modu tam entegrasyon
- ✅ `replay_engine.hpp`: `peek()` ve `next_record()` içindeki `reinterpret_cast` → `memcpy`
- ✅ `backtester.hpp`: `TickDataLoader` içindeki `reinterpret_cast` → `memcpy`  
- ✅ `market_data_recorder.hpp`: `record_count_ptr_` → `record_count_offset_` + `sync_record_count()` via `memcpy`
- ✅ Tüm production-critical kod yollarında `reinterpret_cast` kaldırıldı

**Önceki Düzeltmeler:**
- R5: `ResearchParams`'tan `alignas(64)` kaldırıldı (`-Wpsabi` fix)
- R4: `-Wabi=11` baseline pin (`CMakeLists.txt`)
- R3: Derleme örneğinde `//...\\` → blok yorum (`-Wcomment` fix)
- R2: `--record` moduna `.env` doğrulama ve API key maskeleme eklendi
- R1: `--record` modu placeholder kaldırıldı, tam entegrasyon yazıldı
