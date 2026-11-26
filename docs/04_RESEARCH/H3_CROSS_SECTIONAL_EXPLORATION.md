# H3 ÖN KEŞİF — Çoklu coin, kesitsel REV / MOM / funding sıkışması (yalnız eğitim)

> **Tarih: 2026-11-25.** Yalnız Python veri analizi; C++ kodu, emir ve canlı para yoktur.
> Etiketler: [KOD] / [ÇIKARIM] / [DOĞRULANMADI].
> **Sonuç: 66 hücreden hiçbiri ölçütü geçmedi. H3 ön kaydı önerilmedi; son 12 ay açılmadı.**

## 1. Veri ve evren
- **Kaynak:** Binance USDⓈ-M perp.
  - Günlük mumlar: tüm adaylar (567).
  - 4 s mumlar ve gerçek funding geçmişi: havuz (242 coin).
- **Dönem:** 2023-11-25 … 2025-11-25 08:00 UTC (24 ay). Öncesinde 40 günlük ısınma.
- **Test dönemi hiç indirilmedi.** Bütün istekler `endTime < 1764057600000` ile yapıldı. Diskteki en geç damga `1764057599999` [KOD+veri].
- **Adaylar:**
  - USDT PERPETUAL, `underlyingType=COIN`.
  - TRADING ve **SETTLING** (delist edilmiş, 136 adet) dahil.
  - Stabil ve altın tokenleri hariç; endeks kontratları (BTCDOM, DEFI vb.) hariç.
- **Dinamik evren:** Her gün D için, D'den önceki 30 günün quote hacmine göre ilk **35** coin seçildi. Dün hacmi 0 olan (delist) coinler dışarıda kaldı.
- **Hayatta kalma yanlılığı:** Evren o güne kadarki veriyle seçildi ve delist edilen kontratlar dahil edildi. Bu nedenle yanlılık büyük ölçüde azaltıldı.
  - [DOĞRULANMADI] Çok eski tarihte kaldırılıp `exchangeInfo`'dan silinmiş kontratlar eksik olabilir.
- **Araçlar:**
  - `python/h3_fetch.py` (sha256 `9ab73ec1…92b6`)
  - `python/h3_pool.py` (`e4159b30…759d`)
  - `python/h3_explore.py` (`08b297ff…f48fcf158`)
- Ham çıktı: `docs/04_RESEARCH/h3_train_output.txt`. Veri scratchpad'de, kalıcı değil.

## 2. Model
- **Portföy:** Dolar-nötr. K long × +1/(2K) ve K short × −1/(2K); brüt 1×.
  - Yeniden dengeleme H saatte bir, örtüşmesiz; 00:00 UTC fazı.
  - Giriş/çıkış 4 s kapanışında.
- **REV:** L-saat getirisinde en çok düşen K coin long, en çok çıkan K coin short.
  - L {4, 12, 24}, H {4, 8, 24}, K {2, 3, 5}: 27 hücre.
- **MOM:** Tersi.
  - L {12, 24, 72}, H {4, 8, 24}, K {2, 3, 5}: 27 hücre.
- **FUND:** Skor = sıra(son 24 s gerçek funding toplamı) + sıra(L getirisi). En yüksek K short, en düşük K long.
  - L {24, 72}, H {8, 24}, K {2, 3, 5}: 12 hücre.
- **Funding:** Gerçek ödemeler `(t, t+H]` aralığında, bütün hücrelerde dahil.
- **Maliyet:** `c/2 · Σ|Δw|`, c = 10 ve 14 bps gidiş-dönüş.
- **Önceden sabitlenen ölçüt** (sonuçlardan önce koda yazıldı; M = 66):
  - **İSTİKRARLI** (14 bps'te hepsi birden):
    - net > 0;
    - t ≥ 3,17 (Bonferroni, tek yönlü 0,05/66);
    - iki 12 aylık yarı da > 0;
    - pozitif çeyrek ≥ 5/8;
    - |β_BTC| < 0,3.
  - **Zayıf:** net > 0, t ≥ 2 ve iki yarı da > 0.

## 3. Özet tablo (eğitim, 14 bps aksi belirtilmedikçe)
| Aile | Hücre | Brüt bps/periyot (aralık) | net14 > 0 | net10 > 0 | En iyi t14 | İSTİKRARLI / zayıf |
|---|---|---|---|---|---|---|
| REV | 27 | −17,7 … +8,8 | 0 | 0 | −0,36 | 0 / 0 |
| MOM | 27 | −20,2 … +17,7 | 2 | 3 | +0,61 | 0 / 0 |
| FUND | 12 | −5,3 … +18,9 | 6 | 6 | +0,80 | 0 / 0 |

### En iyi hücreler

| Hücre | net14 bps | Yıllık % (aritmetik) | Sharpe | t | β | Y1 / Y2 % | Ç+ | 24 ay 1.000 USDT → | MDD |
|---|---|---|---|---|---|---|---|---|---|
| FUND L24 H24 K2 | +20,4 | +74 | 0,56 | 0,80 | −0,08 | +13 / +135 | 6/8 | **733** | **%92** |
| FUND L72 H24 K5 | +8,7 | +32 | 0,54 | 0,76 | −0,04 | −20 / +84 | 4/8 | 1.346 | %53 |
| FUND L72 H24 K2 | +10,8 | +40 | 0,30 | 0,42 | −0,07 | −60 / +139 | 3/8 | 362 | %92 |
| MOM L12 H24 K2 | +5,5 | +20 | 0,14 | 0,20 | −0,05 | −111 / +151 | 3/8 | 318 | %93 |
| MOM L12 H8 K2 | +5,0 | +54 | 0,43 | 0,61 | 0,00 | −20 / +129 | 3/8 | 803 | %79 |

## 4. Yorum
- **Kısa ufuklu kesitsel REV:** L4/L12 + H4/H8'de brüt ≈ 0. Devir 1-1,7/periyot, bu yüzden maliyet yılda %100-300 seviyesinde. t14 değerleri −3 … −7 arasında, yani **anlamlı biçimde negatif**. Tek coin H2'de görülen zaman serisi geri dönüşü, kesitsel olarak ortaya çıkmıyor.
- **MOM / FUND:** Pozitif hücrelerin hepsinde t < 1. Y1 ile Y2 arasında işaret değişiyor; tutarlılık yok. 66 hücrede saf şansla beklenen en büyük t ≈ 2,3. Gözlenen en büyük t yalnızca 0,96.
- **FUND ailesinde tek yapısal pozitif kalem funding geliri:** ortalama +6 … +13 bps/gün. Bu, Yol 3'teki taşıma getirisinin kesitsel hâli. Fiyat bileşeni ise gürültü.
- **Aritmetik ortalama yanıltıcı** [KOD+veri]. K = 2 iken tek bir short sıkışması bir günde portföyün %75'ini kaybettirdi. Bileşik getiri negatif, MDD %92. [ÇIKARIM] Gerçekte 1.000 USDT cross margin hesabı bu olayda tasfiye edilirdi.
- **1.000 USDT ile uygulanabilirlik** (kesim günü evreni, mevcut filtreler):
  - K = 2/3/5 için bacak 250/167/100 USDT. `MIN_NOTIONAL` (5/20/50) her coinde sağlanıyor.
  - `LOT_SIZE` yuvarlaması ortalama %1-2, en kötü %29-47 (ör. BTC adımı 0,001 ≈ 87 USDT). Dolar-nötrlük bozuluyor.
  - Model, 4-10 eşzamanlı perp pozisyonu gerektiriyor. **Tek sembollü motor mimarisine uymuyor** (CLAUDE.md §1, §3.5).

## 5. Karar
- Ölçütü geçen aday yok. **H3 ön kaydı önerilmedi.** Son 12 ay açılmadı ve indirilmedi.
- Bu üç aile, bu ızgarayla bir daha aranmaz. Izgarayı genişletip yeniden aramak, çoklu test yanlılığını büyütür.
- Not: Son 12 ay, H2'de SOL/DOGE zaman serisi REV için bir kez açılmıştı. İleride bu dönemde bir test yapılırsa bu kısmi kirlenme yazılmalıdır.
