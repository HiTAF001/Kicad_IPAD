<img src="App/Assets.xcassets/AppIcon.appiconset/icon-1024.png" width="96" align="right" alt="">

# Netloom

**iPad için PCB görüntüleme ve düzenleme** · Seyhan Labs

Netloom, KiCad'in `.kicad_pcb` dosyalarını iPad'de açan ve düzenleyen, `.kicad_sch` şemalarını
gösteren, yerel (native) bir uygulamadır.
Arayüz SwiftUI ile, çizim Metal ile, dosya okuma ve geometri üretimi taşınabilir C++17 ile yazıldı.

> Durum: **Faz 1–6: görüntüleyici, inceleme, düzenleme, Apple Pencil ile pist çizme, tasarım
> kontrolü (DRC), üretim dosyaları, Gerber görüntüleyici ve şema görüntüleyici.** Kartı açar, KiCad
> renkleri ve yazı tipiyle çizer; seçim, net vurgulama, arama, ölçüm, düzenleme, kayıpsız kaydetme,
> ratsnest var. Faz 6 ile KiCad 6–10 şemaları (hiyerarşik sayfalar, netler, sayfalar arası net
> takibi) ve proje klasöründen kart ↔ şema geçişi geldi. Şema düzenleme sonraki fazda.

| Üstten | Alttan görünüm | Katman paneli |
|---|---|---|
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/2-pic-front.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/3-pic-back.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/4-ecc83-layers.png) |
| **Footprint seçimi** | **Net vurgulama** | **Pad / net etiketleri** |
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/6-select-footprint.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/7-net-highlight.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/9-labels.png) |
| **Düzenleme (döndürülmüş footprint + ratsnest)** | **Referans / değer** | **Ölçüm** |
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/11-edit-rotate.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/12-edit-fields.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/8-measure.png) |
| **Kalemle pist çizme** | **Tasarım kontrolü (DRC)** | **Gerber görüntüleyici** |
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/16-route.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/19-turkish-drc.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/20-gerber.png) |
| **Şema görüntüleyici** | **Şemada net vurgulama** | **Alt sayfa** |
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/22-schematic.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/23-schematic-net.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/25-schematic-subsheet.png) |

*Ekran görüntüleri her derlemede CI tarafından iPad simülatöründe çekilir (`ci-screenshots` dalı).*

## Özellikler

- KiCad 4 – 10 (ve 11 geliştirme sürümü) kart dosyaları: pistler, yay pistler, via'lar
  (kör/gömülü/mikro, padstack), tüm pad şekilleri (daire, dikdörtgen, oval, yamuk, yuvarlak köşeli,
  pahlı, özel/custom), bakır bölgeler, çizimler, yazılar, ölçülendirmeler.
- Yazılar Hershey tabanlı kendi stroke yazı tipimizle, KiCad'in ölçüleri ve hizalama kurallarıyla
  çizilir (Türkçe ve diğer Latin harfleri, Yunanca, µ, Ω, °, ±, € dahil);
  TrueType yazılar KiCad'in dosyaya yazdığı `render_cache` çokgenlerinden çizilir.
- KiCad varsayılan renk teması, katman sırası ve yarı saydam maske/pasta görünümü.
- Büyük kartlar: 85 MB'lık `jetson-agx-thor-baseboard` demosu (15 milyon üçgen) açılıyor.
- Katman paneli: katmanları ve nesne türlerini (pist, via, pad, bölge, çizim, yazı, delik) tek tek
  açıp kapatma, "sadece bakır", "sadece ön/arka yüz", alttan görünüm.
- **Seçim ve bilgi kartı:** pad, via, pist, footprint, bakır bölge, çizim veya yazıya dokun;
  net, boyut, delik, katman, footprint, pin işlevi, pist uzunluğu gibi bilgileri gösterir.
  Aynı yere tekrar dokunmak üst üste binen öğeler arasında geçer.
- **Net vurgulama:** seçili öğenin netindeki her şey parlar, gerisi kararır; netin pad/via/pist
  sayısı, toplam pist uzunluğu ve bağlı elemanlar listelenir.
- **Arama:** footprint (R12, U3, 10k) veya net (GND, SDA) bul, oraya git ve vurgula.
- **Ölçüm:** iki noktaya dokun → mesafe, ΔX, ΔY, açı; pad ve via merkezlerine tutunur.
- **Pad numarası ve net adları:** yakınlaştıkça pad'lerin üzerinde numara ve net adı, pistlerin
  üzerinde net adı belirir (KiCad'deki gibi); alttan görünümde de okunur.
- **Düzenleme (Faz 4):**
  - footprint'i parmakla sürükleyip ızgaraya oturtma (0,1 – 2,54 mm), 90° döndürme
  - silme: footprint, pist, via, bakır bölge, çizim, yazı
  - referans ve değer değiştirme, pist genişliği, via çapı ve deliği
  - sınırsız geri al / yinele
  - **Kaydet** (dosyanın üzerine) ve **Farklı kaydet**; kaydedilmemiş değişiklik uyarısı
  - kayıpsız kaydetme: yalnız değiştirilen öğeler yeniden yazılır, dosyanın geri kalanı bayt bayt
    aynı kalır (KiCad'in kendi biçimlendirmesi, yorumlar, sıralama korunur)
  - kilitli öğeler düzenlenmez (KiCad'deki gibi)
- **Ratsnest:** bağlanmamış pad'ler arasında ince çizgiler; her düzenlemeden sonra yeniden
  hesaplanır. Kart bilgisinde eksik bağlantı sayısı görünür.
- **Pist ve via çizme (Faz 5):** Apple Pencil ile (istersen parmakla) bir pad, via veya pist
  ucundan çiz; titrek çizgi temiz 45° piste dönüşür ve bakıra tutunur. Katman, genişlik ve via
  ölçüsü çizim çubuğundan seçilir; başka katmandaki bir pist veya pad'de bitirmek otomatik via
  ekler. İki neti birleştirecek ya da başka bakıra değecek pist eklenmez, kırmızı halkalar nedenini
  gösterir.
- **Tasarım kontrolü (DRC):** açıklık, kısa devre, kesişen pistler, delik açıklığı, kart kenarı,
  pist/via/halka/delik ölçüleri, delik-delik mesafesi, courtyard çakışması, bağlanmamış öğeler,
  sarkan pist/via, bozuk kart kenarı. Kurallar KiCad proje dosyasından (`.kicad_pro`) okunur:
  net sınıfları, kural önem dereceleri, KiCad 5–10 biçimleri. Bulgulara dokunmak kartta o yere
  götürür.
- **Üretim dosyaları (ücretsiz):** Gerber X2 (bakır, maske, pasta, serigrafi, kart kenarı),
  Excellon delik dosyaları (PTH/NPTH, oval delikler), malzeme listesi (BOM, CSV) ve dizgi dosyası
  (CPL); hepsi tek ZIP'te, her üreticinin kabul ettiği biçimde.
- **Gerber görüntüleyici:** herhangi bir CAD programının Gerber ve delik dosyalarını aç (hepsini
  birden seç, klasörünü ya da ZIP'ini) ve sipariş vermeden önce katman katman incele. Katmanlar
  X2 bilgisinden, KiCad / Altium (Protel) / Eagle dosya adlarından bulunur. Aperture makroları,
  delikli aperture'lar, termal, bölgeler, yaylar, step-repeat, "clear" polarite ve negatif
  görüntü desteklenir. Bu kartlar yalnız görüntülenir.
- **Şema görüntüleyici (Faz 6):** KiCad 6 – 10 `.kicad_sch` dosyaları KiCad'in varsayılan
  renkleri, sayfa çerçevesi ve antet bloğuyla çizilir: semboller (çok birimli, De Morgan, aynalı ve
  döndürülmüş), pin adları ve numaraları, teller, veriyolları, bağlantı noktaları, yerel / genel /
  hiyerarşik etiketler, alt sayfa kutuları, notlar ve metin kutuları.
  - **Hiyerarşi:** proje klasörü açılınca bütün alt sayfalar gelir; sayfa listesi, "Üst sayfa",
    sayfa kutusuna dokunup "Sayfayı aç". Aynı alt sayfanın birden çok kopyası ayrı sayfa olarak
    (kendi referanslarıyla) görünür.
  - **Netler:** KiCad'in bağlantı kurallarıyla bütün hiyerarşi boyunca çıkarılır (teller, bağlantı
    noktaları, etiketler, güç sembolleri, sayfa pini ↔ hiyerarşik etiket, vektör ve grup
    veriyolları, bus alias'ları) ve KiCad gibi adlandırılır. Bir öğeye dokununca bilgisi, netini
    vurgulama ve netin devam ettiği diğer sayfalara atlama.
  - **Arama:** bütün sayfalardaki semboller ve netler.
  - **Kart ↔ şema:** klasörde ikisi de varsa araç çubuğundan geçilir; karttaki düzenlemeler korunur.
- **11 dil:** Türkçe, İngilizce, Almanca, Fransızca, İspanyolca, İtalyanca, Fince, Rusça, Japonca,
  Korece, Çince (basitleştirilmiş). Uygulama cihazın diline göre açılır; listede olmayan dillerde
  İngilizce başlar. Bilgi kartındaki öğe açıklamaları ve hata mesajları da çevrilidir.
- Kart bilgisi ekranı (ölçüler, eleman sayıları, KiCad sürümü).
- Uygulamanın içinde iki örnek kart ve bir örnek şema (KiCad demolarından).

## iPad'e kurulum (Mac gerekmez)

GitHub Actions her `main` push'unda imzasız bir `Netloom.ipa` üretir:

- **Releases → "latest"** sayfasında `Netloom.ipa` olarak, ya da
- **Actions → son çalışma → Artifacts → `Netloom-ipa`** olarak indirilebilir.

İmzasız IPA'yı kendi Apple ID'nle imzalayıp yüklemek için Windows'ta iki yol var:

### Sideloadly (en kolayı)

1. Apple'ın sitesinden **iTunes** ve **iCloud**'u kur (Microsoft Store sürümleri değil, web sürümleri).
2. [Sideloadly](https://sideloadly.io)'yi kur, iPad'i USB ile bağla ve "Bu bilgisayara güven" de.
3. `Netloom.ipa`'yı Sideloadly'ye sürükle, Apple ID'ni yaz, **Start**'a bas.
4. iPad'de: **Ayarlar → Gizlilik ve Güvenlik → Geliştirici Modu**'nu aç (iPadOS 16+), cihaz
   yeniden başlar. Sonra **Ayarlar → Genel → VPN ve Cihaz Yönetimi**'nden Apple ID'ne güven.

### AltStore

[AltServer for Windows](https://altstore.io) kur, iPad'e AltStore'u yükle, sonra IPA'yı
AltStore → My Apps → **+** ile seç.

> Ücretsiz Apple ID ile imzalanan uygulamalar **7 gün** çalışır, sonra yeniden imzalamak gerekir
> (Sideloadly/AltStore bunu tek tıkla yapar). Yıllık ücretli geliştirici hesabında süre 1 yıldır.

## Kullanım

| Hareket | İşlev |
|---|---|
| Bir veya iki parmakla sürükle | Kaydır |
| İki parmakla sıkıştır | Yakınlaştır / uzaklaştır |
| Çift dokun | Dokunulan yere 2× yakınlaş |
| İki parmakla tek dokun | Ekrana sığdır |
| Tek dokun | Öğeyi seç ve bilgisini göster (ölçüm modunda: ölçüm noktası) |
| Aynı yere tekrar dokun | Alttaki öğeye geç |
| Trackpad / Magic Keyboard | Kaydırma ve sıkıştırma çalışır |
| **Düzenleme modunda:** seçili footprint'i sürükle | Taşı (ızgaraya oturur); boş yerde sürüklemek kartı kaydırır |

Düzenleme için sağ üstteki kalem simgesine dokun. Düzenleme modunda bir pad'e dokunmak o pad'in
footprint'ini seçer. Üstteki çubukta geri al / yinele, döndür, sil, seçime göre referans/değer,
pist genişliği veya via, ızgara ve kaydet düğmeleri var.

Pist çizmek için düzenleme çubuğundaki kalem ucu düğmesine dokun. Apple Pencil çizer, parmaklar
kartı kaydırır; kalem yoksa tek parmak çizer, iki parmak kaydırır. Çizgiyi bırakınca pist yerine
oturur; geri al ile kaldırılır. Kalkan simgesi tasarım kontrolünü, kutu simgesi üretim
dosyalarını açar.

> Bakır bölge dolguları yeniden hesaplanmaz (KiCad'de de dolgu elle yenilenir). Taşıdığın
> footprint bir bölgenin içindeyse dosyayı KiCad'de açıp dolguyu yenile (**B**). Örnek kartlar
> uygulama paketinin içinde olduğu için yalnız "Farklı kaydet" ile saklanabilir.

Dosyayı **Aç** düğmesiyle (Dosyalar, iCloud Drive, OneDrive, AirDrop ile gelenler) seçebilir ya da
Dosyalar uygulamasında bir `.kicad_pcb` dosyasını paylaş menüsünden Netloom'a gönderebilirsin.
Gerber setleri için aynı düğmeyle dosyaların hepsini birden, klasörlerini ya da ZIP'lerini seç.

Şemalar için en iyisi **proje klasörünü** seçmek: bütün alt sayfalar ve (varsa) kart birlikte
açılır, araç çubuğunda **Kart / Şema** geçişi belirir. Tek bir `.kicad_sch` dosyası da açılır, ama
iPadOS başka dosyalara izin vermediği için yalnız o sayfa görünür. Şemada sol üstteki yığın simgesi
sayfaları listeler; sayfa kutusuna dokunup **Sayfayı aç**, geri dönmek için **Üst sayfa**.

## Mimari

```
core/            Taşınabilir C++17 çekirdek (iOS, Linux, Windows, macOS'ta derlenir)
  src/sexpr.*        S-ifadesi okuyucu (akış halinde, yerel ayardan bağımsız sayı okuma)
  src/board_parser.* .kicad_pcb → kart modeli (KiCad 4 … 11 dosya biçimleri)
  src/stroke_font.*  KiCad stroke yazı tipi ve metin yerleşimi
  src/mesh.*         Kart → katman/nesne gruplarına ayrılmış üçgen ağları (earcut, paralel),
                     öğe başına üçgen aralıkları (vurgulama), pad/net etiketleri
  src/pick.*         Seçim (hit-test), öğe/net açıklamaları, ölçüm için tutunma
  src/document.*     Kayıpsız düzenlenebilir belge: üst düzey öğeler bayt aralıklarıyla,
                     geri al / yinele
  src/edit.*         Düzenlemeler (taşı, döndür, sil, alan, pist genişliği, via) — yalnız ilgili
                     düğümlerin baytlarını değiştirir
  src/connectivity.* Bakır bağlantı analizi (pad/via/pist/bölge) ve ratsnest (MST)
  src/geom.*         Bakır şekilleri, mesafe hesapları, katman başına ızgara dizini
  src/route.*        Kalem çizgisi → 45° pist ve via (sadeleştirme, yön kuantalama, tutunma,
                     kısa devre ve açıklık kontrolü)
  src/drc.*          Tasarım kontrolü (KiCad'in kural anlamlarıyla), .kicad_pro kuralları
  src/fab.*          Gerber X2, Excellon, BOM, dizgi dosyası, ZIP
  src/gerber.*       Gerber / Excellon / ZIP okuyucu → yalnız görüntülenen kart
  src/sch_parser.*   .kicad_sch → şema modeli, sayfa hiyerarşisi (KiCad 6 … 10)
  src/sch_mesh.*     Şema sayfası → üçgen ağları (KiCad'in sembol, pin, etiket ve antet kuralları)
  src/sch_connectivity.* Şema netleri: KiCad'in bağlantı motorunun kuralları (adalar, ad
                     iddiaları, sayfa / genel / port kapsamları, veriyolu üyeleri)
  src/sch_pick.*     Şemada seçim (çizilen üçgenlerle), açıklamalar, vurgulama
  include/kpcore.h   Swift'in kullandığı düz C arayüzü
App/             SwiftUI arayüz + Metal çizici
tools/kpcli.cpp  Masaüstü test aracı: istatistik, PNG çıktı (Metal çiziciyi taklit eder)
samples/         Örnek kartlar ve şema
project.yml      XcodeGen proje tanımı (Xcode projesi CI'da üretilir)
```

Çizim: her grup tek renkli üçgenlerden oluşur. Katmanlar arkadan öne çizilir; bir katman içinde
stencil testi her pikselin bir kez boyanmasını sağlar, bu yüzden yarı saydam katmanlar (maske,
pasta) KiCad'deki gibi görünür. Gerber'deki "clear" polarite de aynı stencil ile çizilir: katmanın
şekilleri sondan başa doğru verilir, silen şekiller tamamen saydam renkle pikselleri sahiplenir.

### Çekirdeği bilgisayarda test etmek

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/kpcli stats samples/pic_programmer.kicad_pcb
./build/kpcli render samples/pic_programmer.kicad_pcb out.png 1600          # üstten
./build/kpcli render samples/pic_programmer.kicad_pcb out-back.png 1600 --back
./build/kpcli pick samples/pic_programmer.kicad_pcb 110.49 78.867            # o noktada ne var
./build/kpcli render samples/pic_programmer.kicad_pcb net.png 1600 --pick 110.49 78.867 --net
./build/kpcli edit samples/pic_programmer.kicad_pcb out.kicad_pcb move C1 2.54 0 rotate C2 90 \
      field C1 Value 470uF delete P101 undo                                  # düzenle, kaydet, yeniden aç
./build/kpcli drc samples/pic_programmer.kicad_pcb                           # tasarım kontrolü
./build/kpcli fab samples/pic_programmer.kicad_pcb out/                      # Gerber, delik, BOM, CPL, ZIP
./build/kpcli render out/ gerber.png 1600                                    # Gerber klasörü, ZIP veya tek dosya
./build/kpcli render samples/pic_programmer.kicad_sch sch.png 1600 --sheet 1  # şemanın 2. sayfası
./build/kpcli sheets samples/pic_programmer.kicad_sch                        # sayfa hiyerarşisi
./build/kpcli netlist samples/pic_programmer.kicad_sch                       # netler ve pinleri
./build/kpcli root path/to/project/                                          # kök sayfa tahmini
ctest --test-dir build
```

Çekirdek, KiCad deposundaki 446 test/demo kartının tamamını (KiCad 4'ten 11 geliştirme sürümüne)
AddressSanitizer/UBSan altında hatasız açıyor. Bozuk dosyalar çökme yerine satır numaralı hata
mesajı veriyor.

Doğrulama (KiCad 7'nin kendi Python API'si referans alınarak):

- **Ratsnest:** KiCad 7'nin okuyabildiği 71 kartta eksik bağlantı sayısı KiCad'in
  `GetUnconnectedCount()` sonucuyla birebir aynı.
- **Düzenleme:** 40 kartta rastgele taşıma/döndürme/alan değişikliği yapıldı; Netloom'un kaydettiği
  dosya, aynı işlemlerin KiCad API'siyle yapılmış haliyle karşılaştırıldı (footprint konumu ve açısı,
  her pad'in konumu ve açısı, referans/değer yazılarının konumu ve açısı): 0 fark.
- Tüm düzenlemeler geri alınınca dosya orijinaliyle bayt bayt aynı (ctest).
- **DRC:** KiCad 7'nin DRC'siyle 274 geometrik ihlal çiftinin 267'si birebir aynı; kalan farklar
  belgelenmiş durumlar (`.kicad_dru` özel kuralları henüz yok, KiCad 7 ile güncel KiCad arasındaki
  davranış farkları).
- **Üretim dosyaları:** bakır, maske, pasta, kart kenarı ve delik dosyaları `kicad-cli`'nin
  çıktısıyla piksel piksel karşılaştırıldı (gerbv ile rasterleştirilerek): aynı; maskede küçük
  yaklaşıklık, serigrafide yalnız yazı tipi glif farkı. Dizgi dosyası 305 elemanda aynı.
- **Şema çizimi:** KiCad 7'nin okuyabildiği 163 sayfa `kicad-cli sch export svg` çıktısıyla piksel
  maskesi olarak karşılaştırıldı: medyan %2,6 eksik, %0,4 fazla mürekkep (fark çoğunlukla yazı tipi).
  482 şema dosyası ASan altında hatasız açılıp her noktasında seçildi.
- **Şema netleri:** KiCad deposundaki 23 referans netlistin 22'si birebir aynı (kalan tek fark
  referans dosyadaki eski tarz bir net adı); KiCad 7'nin `kicad-cli sch export netlist`
  çıktısıyla 112 şemada 2082 net aynı. Kalan farklar KiCad 7'ye özgü: ad verilmemiş (R?)
  semboller, paylaşılan alt sayfalarda sıraya bağlı ad seçimi ve KiCad 7'nin hiyerarşiden geçen
  veriyolu üyelerini bağlamaması (güncel KiCad bağlıyor).
- **Gerber okuyucu:** KiCad'in kendi Gerber'leri (X2, X2'siz, 4.5/4.6 hassasiyet, makrolu/makrosuz,
  Protel uzantıları; mm/inç, LZ/TZ/ondalık delik dosyaları) okunup yeniden yazıldı ve orijinaliyle
  karşılaştırıldı: serigrafi, kenar ve delikler birebir, bakır/maske/pasta yalnız tek piksellik
  kenar farkı. gerbv'nin örnek dosyaları (Altium, Eagle, Mentor, OrCAD, gEDA) da aynı yolla
  denendi.

### Çeviriler

Arayüz metinleri kaynak kodda İngilizcedir; çeviriler tek bir tabloda durur:

- `tools/i18n/ui.tsv` (arayüz) ve `tools/i18n/core.tsv` (çekirdeğin açıklama ve hata metinleri)
- `python3 tools/i18n/generate.py` → `App/Localization/*.lproj/{Localizable,Core}.strings`
- `--check` koddaki her metnin her dile çevrildiğini ve yer tutucuların (`%@`, `{0}`) tuttuğunu
  denetler; CI bunu her derlemede çalıştırır.
- Çekirdek metinlerini masaüstünde denemek için:
  `KPCLI_STRINGS=App/Localization/tr.lproj/Core.strings ./build/kpcli pick ...`
- İkinci görüş için `tools/i18n/deepseek_review.py` çevirileri DeepSeek'e inceletip yalnız
  düzeltmeleri (gerekçeleriyle) yazar. GitHub'da "Translation review (DeepSeek)" iş akışı
  (`DEEPSEEK_API_KEY` sırrı gerekir) ya da yerelde OpenAI uyumlu bir ağ geçidiyle, örneğin
  OmniRoute (PowerShell):

  ```powershell
  $env:OMNIROUTE_API_KEY = "<OmniRoute panelindeki anahtar>"
  python tools\i18n\deepseek_review.py --base-url http://localhost:20128/v1 `
      --key-env OMNIROUTE_API_KEY --model <DeepSeek model kimliği> --report review.md
  python tools\i18n\generate.py
  ```

### Mac'te derlemek (isteğe bağlı)

```sh
brew install xcodegen
xcodegen generate
open Netloom.xcodeproj
```

## Yol haritası

- ~~Faz 3: eleman seçme ve bilgi, net vurgulama, arama, ölçüm aracı, pad/net adları~~ ✓
- ~~Faz 4: düzenleme ve kaydetme (taşıma, döndürme, silme, alanlar, ratsnest)~~ ✓
- ~~Faz 5: kalemle pist ve via çizme, DRC, üretim dosyaları (Gerber/Excellon/BOM/CPL), Gerber görüntüleyici~~ ✓
- ~~Faz 6: şematik görüntüleyici (`.kicad_sch`)~~ ✓
- Faz 7: şematik düzenleme ve PCB'ye aktarma (netlist güncelleme)
- Faz 8: sıfırdan proje: kart kenarı, footprint kütüphanesi, yerleştirme

## Lisans

<img src="branding/seyhan-labs-mark.png" width="72" align="right" alt="Seyhan Labs">

Netloom © 2026 Seyhan Labs. Bu depodaki kaynak kod **GPL-3.0-or-later** ile yayınlanır
(bkz. `LICENSE`). Telif hakkının tamamı Seyhan Labs'e ait olduğu için uygulama ayrıca başka
koşullarla da dağıtılabilir (ör. App Store); ayrıntılar `COPYRIGHT.md` dosyasında. Katkılar
yalnız katkı sözleşmesiyle (CLA) kabul edilir, bkz. `CONTRIBUTING.md`.

Üçüncü parti parçalar (`THIRD_PARTY_NOTICES.md`):

- Hershey yazı tipi verisi (atıfla serbest kullanım, `core/third_party/hershey/NOTICE`)
- `core/third_party/earcut.hpp`: Mapbox earcut (ISC)
- `tools/stb_image_write.h`: stb (MIT / kamu malı, yalnız masaüstü test aracı)
- `samples/*.kicad_pcb`: KiCad demo kartları (CC BY-SA 4.0, KiCad geliştiricileri)

Netloom, KiCad projesiyle bağlantılı değildir. "KiCad", LF Projects, LLC'nin tescilli markasıdır.
