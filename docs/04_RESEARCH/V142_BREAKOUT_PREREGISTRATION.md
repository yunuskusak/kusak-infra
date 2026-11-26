# v14.2 ÖN KAYIT — "Avcı": nadir ekstrem kırılımlar, 1-15 dk ufuk

> **Ön kayıt tarihi: 2026-11-24.** Protokol sonuçlar görülmeden yazıldı. Yalnız veri analizidir; C++ kodu, emir ve canlı para yoktur.
> Etiketler: [KOD] / [ÇIKARIM] / [DOĞRULANMADI]. Bu çalışma bir **keşif**tir: test setini geçen bir hücre bile Kapı 2 (gerçek dolum) ve canlı doğrulama olmadan kanıt sayılmaz.

## 1. Soru
Nadir ve ekstrem fiyat hareketlerinden sonraki 1-15 dakikada, gidiş-dönüş maliyetini aşan, yöne bağlı ve tekrarlanabilir bir hareket var mı? Hedef sıklık saatte 1 veya günde 2 işlem.

## 2. Veri
- Kaynak: `data.binance.vision`, spot 1 dk klines. Semboller BTCUSDT ve PENDLEUSDT. CHECKSUM (sha256) doğrulanır.
- Dönem: 2025-11 … 2026-10, 12 tam ay.
  - **Eğitim:** 2025-11 … 2026-06 (8 ay).
  - **Test:** 2026-07 … 2026-10 (4 ay). Test seti, eğitimde seçim yapılana kadar açılmaz.
- Kendi kaydımız (1 saatlik OBI CSV) bu ufuk için yetersiz; kullanılmaz.

## 3. Sinyal (bar t kapanışında, yalnız geçmiş veriyle)
- `r_L = ln(C_t / C_{t-L})`, L ∈ {1, 5, 15} dk.
- `σ1` = son 1440 dakikalık 1 dk log getirilerinin standart sapması. `z = r_L / (σ1·√L)`.
- Olay: `|z| ≥ k`. k, **eğitim setinde** hedef sıklığı verecek |z| kantilidir: f ∈ {saatte 1, günde 2}, örtüşme kuralından önce. Aynı k test setinde değiştirilmeden kullanılır.
- Yön: **MOM** (hareket yönünde) ve **REV** (ters yönde).

## 4. İşlem modeli
- Giriş: bar t+1'in açılışı. Çıkış: bar t+H'nin kapanışı. H ∈ {1, 5, 10, 15} dk.
- Örtüşme yok: açık işlem varken yeni olay alınmaz.
- **Maliyet (birincil): taker + taker, 2×10 bps + 1,25×spread.**
  - Spread, 2026-11-24 ölçümünden alınır: BTC ≈ 0, PENDLE 8 bps. Maliyet BTC **20 bps**, PENDLE **30 bps**.
  - Duyarlılık: BNB ücreti 2×7,5 bps.
  - [ÇIKARIM] Ekstrem anlarda spread ve kayma büyür; model iyimserdir.
- Izgara: 3 L × 2 f × 2 yön × 4 H = 48 hücre/sembol, toplam **M = 96**.

## 5. Karar kuralı
1. **Eğitim seçimi:** net EV > 0, N ≥ 30 ve tek yönlü t-testinde p_bonf = min(1, p·96) < 0,05.
2. **Test:** seçilen her hücre, değiştirilmeden test setinde değerlendirilir. Test eşiği p < 0,05 / (seçilen hücre sayısı), ayrıca net EV > 0 ve N ≥ 15.
3. Test setinin 4 ayının en az 3'ünde net EV > 0.
4. Bu şartları geçen hücre yoksa sonuç: **"bu sinyal ailesinde maliyet sonrası avantaj bulunamadı"**. Ardından eşik, gösterge veya pencere değiştirerek yeniden arama yapılmaz.

## 6. Raporlanacaklar
Hücre başına N, brüt ve net ortalama, net kazanma oranı, t ve p; ay bazında net; eğitim ve test yan yana. Başarısız hücreler dahil her şey raporlanır.

## 7. Değişiklik günlüğü
| Tarih | Değişiklik | Gerekçe |
|---|---|---|
| 2026-11-24 | Ön kayıt oluşturuldu | — |
| 2026-11-24 | **Sonuç: eğitimde seçilen hücre yok, test seti AÇILMADI.** Ayrıntı §8 | Protokol §5.4 |

## 8. Sonuç (2026-11-24)
- Veri: her sembol için 525.600 bar, zaman boşluğu 0, 24 zip'in sha256'sı doğrulandı.
- Araç: `~/research_v142/bin/breakout.py` (WSL, repo dışı; sha256 `909c67b1…1309`). Çıktı: `~/research_v142/out_v1.txt`.
- **96 hücrenin hepsinde eğitim net EV < 0; p_bonf = 1,000.**
  - **BTC:** brüt hareket her hücrede −3,7 … +3,7 bps; maliyet 20 bps. Ekstrem hareketten sonra yöne bağlı bir hareket görülmedi.
  - **PENDLE:** en yakın hücreler "günde 2" sıklığında ve REV yönünde, yani ekstrem hareket sonrası geri dönüş:
    - L=15, H=15: brüt **+23,4** / net −6,6 bps (BNB ile −1,6), N=98;
    - L=15, H=10: +22,3 / −7,7, N=112;
    - L=5, H=10: +15,5 / −14,5, N=211.

    MOM yönünde bu hücrelerin tersi (−12 … −23 bps) görüldü. [ÇIKARIM] Ekstrem PENDLE hareketlerinde kısmi geri dönüş eğilimi var, ama birincil maliyeti karşılamıyor; ekstrem anlardaki gerçek spread ve kayma modelden yüksek olacağı için gerçekte daha da kötü olabilir.
- Protokol gereği bu sinyal ailesinde yeniden arama yapılmaz. Test seti (2026-07 … 2026-10) dokunulmamış durumda.

## 9. Ek ön kayıt — H1: tek hipotez, tek test (2026-11-24, test seti açılmadan önce)
Proje sahibi kararı: §8'deki eğitim gözleminden türetilen **tek** bir hipotez, dokunulmamış test setinde **bir kez** sınanır. **Geçmezse kesin olarak Aşama 5B'ye geçilir.** Sonuç ne olursa olsun bu hipotez ve test seti ikinci kez kullanılmaz.
- **H1:** PENDLEUSDT'de, günde 1'den nadir ekstrem 15 dakikalık hareketlerden sonra ters yönde (REV) açılan ve 15 dakika tutulan işlemin, maliyet sonrası beklenen getirisi sıfırdan büyüktür.
- **Sabit parametreler:**
  - Sembol PENDLEUSDT. L = 15, yön REV, H = 15.
  - **f = günde 1** (1/1440 olay/dk). k, eğitim setinde |z|'nin 1 − 1/1440 kantili olarak hesaplanır.
  - Giriş, çıkış, örtüşme kuralı, σ1 penceresi ve veri §3-§4 ile aynıdır.
  - Gerekçe: L ve H, eğitimde en yüksek brüt değeri veren hücreden (§8) alındı. f, "nadirlik arttıkça geri dönüş büyüyor" gözleminin bir adım ötesi.
- **Maliyet (birincil, karar için):** 2×10 + 1,25×8 = **30 bps**. BNB (25 bps) yalnız bilgi amaçlıdır.
- **GEÇME şartı (hepsi birden, test seti 2026-07 … 2026-10):**
  1. N ≥ 15;
  2. net ortalama > 0;
  3. tek yönlü Student-t p < 0,05 (tek test olduğu için düzeltme yok);
  4. 4 ayın en az 3'ünde ay ortalaması net > 0. Olay olmayan ay "pozitif değil" sayılır.
- Eğitim setindeki aynı hücre yalnız bilgi olarak raporlanır, karara girmez.
- [ÇIKARIM] Güç sınırlı: N küçük olacağı için gerçek bir kenar da kaçırılabilir. Bu risk önceden kabul edilmiştir.

### 9.1 H1 sonucu (2026-11-24): **KALDI → Aşama 5B**
- Araç: `~/research_v142/bin/h1_test.py` (sha256 `d59bf5dd…af07`; `breakout.py` fonksiyonları birebir). Çıktı: `~/research_v142/out_h1.txt`. k = 6,020.
- Eğitim (bilgi): N=53, brüt +24,0, net −6,0 bps, p=0,58.
- **Test (karar):** N=21, **brüt −4,3, net −34,3 bps**, net kazanma %33, t=−1,33, p=0,90. Ay bazında net: 07 −99,5 / 08 +16,0 / 09 −35,1 / 10 −53,4 (1/4 pozitif).
- Şartlar: N≥15 EVET; net>0 HAYIR; p<0,05 HAYIR; ay+ ≥3 HAYIR.
- [ÇIKARIM] Eğitimde görülen geri dönüş eğilimi test döneminde tamamen kayboldu (brüt +24 → −4). Eğitimdeki aylık dağılım da çok oynaktı (−101 … +84). Eğilim büyük olasılıkla döneme özgü gürültüydü.
- Ön kayıt gereği H1 ve bu test seti bir daha kullanılmaz. **Karar: Aşama 5B.**
