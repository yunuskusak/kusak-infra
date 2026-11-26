# H2 ÖN KAYIT — 4 saatlik sert hareket sonrası geri dönüş (REV), maker giriş

> **Ön kayıt tarihi: 2026-11-25.** Protokol, test dönemi açılmadan yazıldı. Yalnız veri analizidir; C++ kodu, emir ve canlı para yoktur.
> Etiketler: [KOD] / [ÇIKARIM] / [DOĞRULANMADI]. Test GEÇSE bile Kapı 2 (gerçek dolum, sahte borsa/testnet) ve üç P0'ın kapanması olmadan canlı işlem yapılmaz (CLAUDE.md §2).

## 1. Soru ve kaynak
- **Soru:** SOL ve DOGE'da 4 saatlik sert bir hareketten sonra ters yönde açılan ve 4 saat tutulan işlem, maker giriş maliyetiyle (12 bps) net pozitif mi?
- **Kaynak:** 2026-11-25 "20 bps engeli" analizi (`PROGRESS.md`). İlk 24 ayda 48 hücrelik keşif ızgarası kuruldu: 6 varlık × L{4,24} × H{1,4} × MOM/REV. REV yönü tutarlı biçimde pozitif brüt verdi. En iyi iki hücre SOL ve DOGE L4/H4 REV, brüt ≈ +20 bps.
- **Seçim yanlılığı:** Hipotez 48 hücrenin en iyisinden seçildi; eğitim sonucu bu yüzden iyimser. Karar yalnız dokunulmamış test dönemine dayanır. Tek test yapılır, düzeltme gerekmez.

## 2. Veri
- Kaynak: Binance spot REST `/api/v3/klines`, 1 saatlik mumlar, SOLUSDT ve DOGEUSDT. 2026-11-25'te çekildi. Her sembol için 26.280 bar, zaman boşluğu 0.
- sha256:
  - SOL `cd3521b0…67ca`
  - DOGE `5662e372…4ffb4d`
- Dosyalar scratchpad'de (`k1h_spot_*.json`), kalıcı değil. Kapanmış barlar yeniden çekilebilir.
- **Eğitim:** 2023-11-26 08:00 … 2025-11-25 08:00 UTC (24 ay).
- **Test:** 2025-11-25 08:00 … 2026-11-25 06:00 UTC (12 ay). Son, kapanmamış bar kullanılmaz.
- **Açıklama:** Test dönemi daha önce yalnız koşulsuz oynaklık (σ, E|r|) tablosu için okundu. Sinyal-getiri ilişkisi test döneminde hiç hesaplanmadı. Test dönemi V142/H1 test setiyle (2026-07 … 10) çakışıyor, ama varlıklar (SOL, DOGE) ve ufuk farklı.

## 3. Sinyal ve işlem modeli (sabit)
- **Izgara:** i ∈ {4, 8, 12, …}; bar indeksine göre 4 saatlik adım, örtüşme yok.
- **Sinyal:** `s = C_i / C_{i-4} − 1`. Olay koşulu `|s| ≥ thr`.
  - `thr`, eğitimdeki |s| dağılımının %90 kantilidir. Testte değiştirilmez:
    - **SOL 306,39 bps**
    - **DOGE 316,61 bps**
- **Yön:** REV. `s > 0` ise SHORT, `s < 0` ise LONG. **Birincil hipotez iki yönlüdür.**
- **[A] Kapanış-kapanış:** giriş `C_i`, çıkış `C_{i+4}`. `brüt = yön·(C_{i+4}/C_i − 1)`.
- **[B] Maker dolum modeli:**
  - Limit emir `C_i` fiyatına konur.
  - Dolum koşulu: LONG için `Low_{i+1} < C_i`, SHORT için `High_{i+1} > C_i` (kesin aşım).
  - Dolmazsa işlem yoktur. Çıkış `C_{i+4}`.
  - [ÇIKARIM] Eğitimde dolum oranı %99,3. 1 saatlik çözünürlük kuyruk sırasını ve ters seçilimi ölçemez; B, A'dan zayıf bir ölçüdür.
- **Maliyet (birincil, karar için): 12 bps / gidiş-dönüş.** Bu değer iki yürütme yolunu da muhafazakâr biçimde kapsar:
  - FDUSD spot: maker 0 + taker 10 + çıkış yarım spread/kayma ~2. Yalnız LONG yapılabilir.
  - USDⓈ-M perp: maker 2 + taker 5 + ~1 ≈ 8. İki yön de yapılabilir.
  - Ücretlerin kaynağı ikincil; hesaba özgü oran [DOĞRULANMADI].
- **Bilgi (karara girmez):** 7 bps ve 20 bps maliyet; yalnız LONG ve yalnız SHORT alt kümeleri.

## 4. Karar kuralı — GEÇME şartı (hepsi birden, test dönemi)
| Kapı | Şart |
|---|---|
| G1 | A'da işlem sayısı N ≥ 100 |
| G2 | A: net ortalama > 0 **ve** tek yönlü Student-t p < 0,05 |
| G3 | B: net ortalama > 0 **ve** tek yönlü Student-t p < 0,05 |
| G4 | B: test döneminin en iyi ayı çıkarıldığında net ortalama > 0 |

- **t-testi birimi:** 4 saatlik zaman dilimi. Aynı dilimde SOL ve DOGE işlemleri birlikte açılırsa ortalamaları tek gözlem sayılır, çünkü iki varlık birlikte hareket eder.
- **Kaldı ise:** "SOL/DOGE 4 s REV ailesinde 12 bps maliyet sonrası avantaj bulunamadı" denir. Eşik, L, H veya varlık değiştirilerek yeniden arama yapılmaz. Bu test dönemi ikinci kez kullanılmaz.
- **Geçti ise:** bu bir kanıt adayıdır, kanıt değildir. Sıradaki adım ileriye dönük kağıt/dry-run gözlemi ve Kapı 2'dir. P0'lar kapanmadan `dry_run=false` yoktur.

## 5. Güç (önceden kabul edilen risk)
- Eğitim A havuzu: N=876, net +8,07 bps, t=+1,04, p=0,15. Eğitimin kendisi bile p<0,05'i sağlamıyor.
- [ÇIKARIM] Etki gerçek ve aynı büyüklükte olsa bile 12 aylık testte beklenen t ≈ 1,04·√(1/2) ≈ 0,74. G2'yi geçme olasılığı yaklaşık **%18**.
- Sonuç: **KALDI**, "etki yok" demek değildir; yalnız "kanıtlanamadı" demektir. **GEÇTİ** ise anlamlıdır.
- Proje sahibi testin hemen çalıştırılmasını istedi (2026-11-25). Bu risk kabul edilmiştir.

## 6. Eğitim sonucu (bilgi, karara girmez)
- Araç: `python/h2_reversal_test.py` (sha256 `ff86c763…d1fb`, `train` modu).
- **A havuz:** N=876, brüt +20,07, net +8,07, p=0,15.
- **B havuz:** N=870, net +6,96, p=0,18.
- **B LONG:** N=439, net +22,91, p=0,08.
- **B SHORT:** N=431, net −9,28.
- Pozitif ay: 11/25.

## 7. Değişiklik günlüğü
| Tarih | Değişiklik | Gerekçe |
|---|---|---|
| 2026-11-25 | Ön kayıt oluşturuldu | — |
| 2026-11-25 (test öncesi) | G4 "pozitif ay ≥ 7/12" yerine "en iyi ay hariç net > 0" | Eğitimin kendisi 11/25 pozitif ay verdi. Ay başına ~35 işlemle ay ortalamasının standart sapması (~50 bps) etkinin (~8 bps) çok üstünde; ölçüt yazı-tura gibi çalışıyordu. Yeni ölçüt tek ayın sonucu sürüklemesini yakalar. Değişiklik eğitim verisine bakılarak, test açılmadan yapıldı. |
| 2026-11-25 (test öncesi) | Birincil hipotez iki yönlü kaldı; LONG-only yalnız bilgi | LONG'un eğitimde daha iyi görünmesi sonradan seçim olurdu. |
| 2026-11-25 | **Test çalıştırıldı: KALDI** (§8) | Protokol §4 |

## 8. Test sonucu (2026-11-25): **KALDI**
- Araç: `python/h2_reversal_test.py`, `test` modu. Aracın ve verinin sha256'sı çalıştırmadan hemen önce yeniden doğrulandı (§2, §6 ile aynı). Çıktı scratchpad'de `out_h2_test.txt`, kalıcı değil; değerler aşağıda.
- Dolum oranı (B): 189/197 = %95,9.
- **A havuz (G2):** N=197 işlem / 148 dilim, brüt +17,99, işlem başı net +5,99 bps, kazanma %55,8.
  - Dilim bazında t = −0,12, p = 0,55.
  - [ÇIKARIM] İşlem başı ortalama pozitif, ama ön kayıtlı birim olan 4 s dilim ortalaması ≈ 0 / hafif negatif. Pozitif katkı, iki varlığın birlikte tetiklendiği az sayıda dilimden geliyor.
- **B havuz (G3):** N=189, brüt +14,04, net +2,04 bps, t = −0,43, p = 0,67.
- **Ay bazında B net (bps):**

  | 2025-11 | 2025-12 | 2026-01 | 2026-02 | 2026-03 | 2026-04 | 2026-05 | 2026-06 | 2026-07 | 2026-08 | 2026-09 | 2026-10 | 2026-11 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | −6,4 | −16,4 | −8,0 | −104,8 | +21,4 | −34,2 | +68,3 | +86,0 | +5,6 | +66,0 | −44,9 | +112,8 | +37,2 |

  - 2025-11 ve 2026-11 kısmi aylar.
  - En iyi ay (2026-10) çıkarılınca net **−5,47 bps** (N=177).
- **Kapılar:**
  - G1 (N ≥ 100) EVET.
  - G2 HAYIR (p = 0,55).
  - G3 HAYIR (p = 0,67).
  - G4 HAYIR (−5,47).
- **Bilgi:**
  - 7 bps maliyetle A net +10,99 (p = 0,43), B net +7,04 (p = 0,56).
  - 20 bps maliyetle A −2,01, B −5,96.
  - B LONG: net −8,48 (N=99). B SHORT: net +13,61 (N=90).
  - Eğitimde LONG +22,9 / SHORT −9,3 idi; testte işaretler **ters döndü**. [ÇIKARIM] Bu, yön asimetrisinin gürültü olduğunu gösteriyor. Brüt geri dönüş (+14…+18 bps) eğitimdeki büyüklüğe (+19…+20) yakın kaldı; istatistiksel olarak ayırt edilemiyor.
- **Karar (protokol §4):** "SOL/DOGE 4 s REV ailesinde 12 bps maliyet sonrası avantaj bulunamadı". Bu aile ve bu test dönemi bir daha kullanılmaz. §5 gereği bu sonuç "etki yok" değil, "kanıtlanamadı" demektir.
