# KiPad: iPad için KiCad kart görüntüleyici

KiPad, KiCad'in `.kicad_pcb` dosyalarını iPad'de açan, yerel (native) bir uygulamadır.
Arayüz SwiftUI ile, çizim Metal ile, dosya okuma ve geometri üretimi taşınabilir C++17 ile yazıldı.

> Durum: **Faz 1 + 2: görüntüleyici.** Kartı açar, KiCad renkleri ve yazı tipiyle çizer;
> kaydırma, yakınlaştırma, katman ve nesne gizleme, alttan görünüm ve kart bilgisi var.
> Düzenleme (taşıma, pist çizme, DRC) henüz yok.

| Üstten | Alttan görünüm | Katman paneli |
|---|---|---|
| ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/2-pic-front.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/3-pic-back.png) | ![](https://raw.githubusercontent.com/HiTAF001/Kicad_IPAD/ci-screenshots/4-ecc83-layers.png) |

*Ekran görüntüleri her derlemede CI tarafından iPad simülatöründe çekilir (`ci-screenshots` dalı).*

## Özellikler

- KiCad 4 – 10 (ve 11 geliştirme sürümü) kart dosyaları: pistler, yay pistler, via'lar
  (kör/gömülü/mikro, padstack), tüm pad şekilleri (daire, dikdörtgen, oval, yamuk, yuvarlak köşeli,
  pahlı, özel/custom), bakır bölgeler, çizimler, yazılar, ölçülendirmeler.
- Yazılar KiCad'in kendi stroke yazı tipiyle (newstroke), KiCad'in hizalama kurallarıyla çizilir;
  TrueType yazılar KiCad'in dosyaya yazdığı `render_cache` çokgenlerinden çizilir.
- KiCad varsayılan renk teması, katman sırası ve yarı saydam maske/pasta görünümü.
- Büyük kartlar: 85 MB'lık `jetson-agx-thor-baseboard` demosu (15 milyon üçgen) açılıyor.
- Katman paneli: katmanları ve nesne türlerini (pist, via, pad, bölge, çizim, yazı, delik) tek tek
  açıp kapatma, "sadece bakır", "sadece ön/arka yüz", alttan görünüm.
- Tek dokunuşta koordinat okuma (mm), kart bilgisi ekranı (ölçüler, eleman sayıları, KiCad sürümü).
- Uygulamanın içinde iki örnek kart (KiCad demolarından).

## iPad'e kurulum (Mac gerekmez)

GitHub Actions her `main` push'unda imzasız bir `KiPad.ipa` üretir:

- **Releases → "latest"** sayfasında `KiPad.ipa` olarak, ya da
- **Actions → son çalışma → Artifacts → `KiPad-ipa`** olarak indirilebilir.

İmzasız IPA'yı kendi Apple ID'nle imzalayıp yüklemek için Windows'ta iki yol var:

### Sideloadly (en kolayı)

1. Apple'ın sitesinden **iTunes** ve **iCloud**'u kur (Microsoft Store sürümleri değil, web sürümleri).
2. [Sideloadly](https://sideloadly.io)'yi kur, iPad'i USB ile bağla ve "Bu bilgisayara güven" de.
3. `KiPad.ipa`'yı Sideloadly'ye sürükle, Apple ID'ni yaz, **Start**'a bas.
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
| Tek dokun | O noktanın koordinatını (mm) göster |
| Trackpad / Magic Keyboard | Kaydırma ve sıkıştırma çalışır |

Dosyayı **Aç** düğmesiyle (Dosyalar, iCloud Drive, OneDrive, AirDrop ile gelenler) seçebilir ya da
Dosyalar uygulamasında bir `.kicad_pcb` dosyasını paylaş menüsünden KiPad'e gönderebilirsin.

## Mimari

```
core/            Taşınabilir C++17 çekirdek (iOS, Linux, Windows, macOS'ta derlenir)
  src/sexpr.*        S-ifadesi okuyucu (akış halinde, yerel ayardan bağımsız sayı okuma)
  src/board_parser.* .kicad_pcb → kart modeli (KiCad 4 … 11 dosya biçimleri)
  src/stroke_font.*  KiCad stroke yazı tipi ve metin yerleşimi
  src/mesh.*         Kart → katman/nesne gruplarına ayrılmış üçgen ağları (earcut, paralel)
  include/kpcore.h   Swift'in kullandığı düz C arayüzü
App/             SwiftUI arayüz + Metal çizici
tools/kpcli.cpp  Masaüstü test aracı: istatistik, PNG çıktı (Metal çiziciyi taklit eder)
samples/         Örnek kartlar
project.yml      XcodeGen proje tanımı (Xcode projesi CI'da üretilir)
```

Çizim: her grup tek renkli üçgenlerden oluşur. Katmanlar arkadan öne çizilir; bir katman içinde
stencil testi her pikselin bir kez boyanmasını sağlar, bu yüzden yarı saydam katmanlar (maske,
pasta) KiCad'deki gibi görünür.

### Çekirdeği bilgisayarda test etmek

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/kpcli stats samples/pic_programmer.kicad_pcb
./build/kpcli render samples/pic_programmer.kicad_pcb out.png 1600          # üstten
./build/kpcli render samples/pic_programmer.kicad_pcb out-back.png 1600 --back
ctest --test-dir build
```

Çekirdek, KiCad deposundaki 446 test/demo kartının tamamını (KiCad 4'ten 11 geliştirme sürümüne)
AddressSanitizer/UBSan altında hatasız açıyor. Bozuk dosyalar çökme yerine satır numaralı hata
mesajı veriyor.

### Mac'te derlemek (isteğe bağlı)

```sh
brew install xcodegen
xcodegen generate
open KiPad.xcodeproj
```

## Yol haritası

- Faz 3: eleman seçme ve bilgi (footprint, pad, net vurgulama), ölçüm aracı, pad/net adları
- Faz 4: düzenleme ve kaydetme (taşıma, döndürme, özellik değiştirme)
- Faz 5: pist çizme ve DRC, dokunmatik için yeniden tasarlanmış

## Lisans

KiPad **GPL-3.0-or-later** ile lisanslıdır (bkz. `LICENSE`). İçerdiği üçüncü parti parçalar:

- `core/src/newstroke_font.cpp`: KiCad'in stroke yazı tipi verisi (GPL-2.0-or-later;
  CJK glifleri Source Han Sans'tan, SIL OFL 1.1)
- `core/third_party/earcut.hpp`: Mapbox earcut (ISC)
- `tools/stb_image_write.h`: stb (MIT / kamu malı)
- `samples/*.kicad_pcb`: KiCad demo kartları (CC BY-SA 4.0, KiCad geliştiricileri)

KiPad, KiCad projesiyle bağlantılı değildir. "KiCad" adı KiCad projesine aittir.
