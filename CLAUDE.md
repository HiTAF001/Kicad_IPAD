# Netloom — Claude Code devir notu

Bu dosya Claude Code'a yeni oturumda bağlam verir (otomatik okunur). Son güncelleme: 2026-11-04,
sürüm 0.6.0, `main` = Faz 6 bitti, Faz 7 yarım (ayrı dallarda).

## Kullanıcı tercihleri (önce bunlar)

- İletişim **Türkçe**. Kod, yorumlar, commit mesajları ve arayüz kaynak metinleri **İngilizce**.
- Dosya değiştirirken **eksiksiz, yapıştırılabilir** dosya ver; parça parça yama verme.
- Önce **hataları listele**, sonra düzeltmeleri.
- Adım adım ilerle; abartılı övgü yok; iddiaları ölçüm/test/kaynakla destekle.
- Windows tarafındaki komutlar **PowerShell** ile (kullanıcının PC'si: Windows, kullanıcı `fatih`).
- Token tasarrufu için uygunsa alt ajan kullan, ama ajan çıktısını doğrulamadan "bitti" deme.
- API anahtarlarını sohbete yapıştırtma; yerel ortam değişkeni ya da repo sırrı olarak tutulur.

## Proje

Netloom (Seyhan Labs): iPad için KiCad PCB/şema görüntüleyici ve düzenleyici.
SwiftUI + Metal arayüz, kendi yazdığımız taşınabilir **C++17 çekirdek** (KiCad kodu kopyalanmaz —
GPL/çift lisans nedeniyle; davranışı anlamak için KiCad kaynağını okumak serbest).

- Repo: `github.com/HiTAF001/Kicad_IPAD`, dal `main`. Lisans GPL-3.0-or-later + Seyhan Labs çift lisans.
- Ayrıntılı mimari ve doğrulama sonuçları: `README.md` ("Mimari", "Çekirdeği bilgisayarda test etmek").
- Uygulama ikonu: Seyhan Labs işareti (`branding/seyhan-labs-mark.png`, lacivert zemin) —
  bundan sonra hep bu.

```
core/src/          C++ çekirdek: sexpr, board_parser, mesh, pick, document/edit (kayıpsız, undo),
                   connectivity, route, drc, fab, gerber, sch_parser, sch_mesh,
                   sch_connectivity (KiCad master bağlantı motorunun kuralları), sch_pick, i18n
core/include/kpcore.h   Swift'in kullandığı düz C arayüzü (kp_*)
App/               SwiftUI + Metal (LoadedBoard, BoardStore, ContentView, BoardRenderer, ...)
tools/kpcli.cpp    Masaüstü test aracı (stats, render, pick, edit, drc, fab, sheets, netlist, root, picksweep)
tools/i18n/        ui.tsv + core.tsv (çeviri tablosu), generate.py, deepseek_review.py
tools/oracle/      KiCad'e karşı doğrulama betikleri (aşağıda)
samples/           pic_programmer.{kicad_pcb,kicad_sch}, pic_sockets.kicad_sch, ecc83-pp.kicad_pcb
project.yml        XcodeGen tanımı (MARKETING_VERSION 0.6.0); Xcode projesi CI'da üretilir
```

## Derleme ve test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure          # load_samples, edit_undo_roundtrip, fab_gerber_roundtrip
python3 tools/i18n/generate.py --check && python3 tools/i18n/generate.py && git diff --exit-code App/Localization

# ASan/UBSan (seçenek adı NETLOOM_SANITIZE — eski KIPAD_SANITIZE YOK, o ad sessizce yok sayılır)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNETLOOM_SANITIZE=ON && cmake --build build-asan
```

Sık kullanılan: `./build/kpcli netlist samples/pic_programmer.kicad_sch`,
`./build/kpcli render samples/pic_programmer.kicad_sch sch.png 1600 --sheet 1`,
`KPCLI_STRINGS=App/Localization/tr.lproj/Core.strings ./build/kpcli pick ...` (Türkçe çekirdek metinleri).

### Doğrulama betikleri (`tools/oracle/`)

Yollar `NETLOOM_WORK` ortam değişkeniyle verilir (KiCad deposu klonu, kicad-cli çıktıları vb.).
- `sch_net_compare.py` — şema netlerini KiCad referans netlistleriyle / `kicad-cli sch export netlist`
  ile karşılaştırır (son durum: master referanslarında 22/23, KiCad 7 ile 106/112; farklar README'de).
- `sch_compare.py` — şema çizimini `kicad-cli sch export svg` ile piksel maskesi olarak karşılaştırır.
- `drc_compare.py`, `drc_pairs.py`, `kicad_drc*.py`, `kicad_unconnected.py` — DRC / ratsnest.
- `fab_compare.py`, `gerber_roundtrip.py`, `gerbv_corpus.py` — üretim dosyaları ve Gerber okuyucu.
- `verify_edit.py`, `edit_sweep.py`, `route_test.py`, `route_viz.py`, `add_track_test.py` — düzenleme/rota.
- `asan_corpus.sh` — tüm korpusu ASan derlemesiyle açar (son: 451 kart + 482 şema temiz).
- `apply_review.py` — tarayıcıda toplanan çeviri düzeltmelerini TSV'ye uygular.
Bulut oturumunda KiCad 7 (`kicad-cli`, `pcbnew` Python) ve KiCad deposu kurulmuştu; yerelde
gerekirse yeniden kur. Oracle sonuçları hep bir **referansa** (KiCad'in kendi çıktısı) dayanmalı.

## Kurallar

- C++ stili KiCad'e benzer: `aParam`, `m_member`, parantez içinde boşluk `f( x )`, 4 boşluk,
  her `if`/`for` sonrası boş satır. Sınıf/işlev başına kısa İngilizce açıklama.
- Yeni arayüz metni → `ui.tsv`; çekirdek metni (Tr("...")) → `core.tsv`. **10 dil** (tr, de, fr, es,
  it, fi, ru, ja, ko, zh-Hans) + İngilizce kaynak = 11. Ayırıcı ` ‖ `. Sonda boşluk olan anahtar
  olmaz (ör. `Tr("File") + ": "`). Yer tutucular `%@`, `%lld`, `{0}` her dilde aynı olmalı.
- Ağ adları KiCad gibi kaçışlı tutulur (`/` → `{slash}`); gösterirken `DisplayNetName` çözer.
- Şema "kartında": net indeksi = şema net indeksi + 1 (0 = yok), "footprint" = geçerli sayfanın
  sembolleri; seçim çizilen üçgenlerle (mesh bırakılmaz, `release_mesh` şemada atlanır).
- Commit mesajı İngilizce, kısa başlık + gövde. Push'tan önce ctest + `generate.py --check`.

## CI

`.github/workflows/build.yml`: Linux çekirdek testleri + çeviri kontrolü + önizlemeler; macOS'ta
imzasız IPA (`latest` ön sürümüne yüklenir); iPad simülatöründe 27 ekran görüntüsü
(`-NetloomOpenSample`, `-NetloomDemo`, `-NetloomPanel` argümanlarıyla; yeni ekran → `shoot` satırı ekle).
- Ekran görüntüleri `ci-screenshots` dalına force-push edilir: `git fetch origin +ci-screenshots:ci-screenshots`.
- `gh release` (GraphQL) bu ortamda engelliydi; REST kullan:
  `gh api -H "Accept: application/octet-stream" repos/HiTAF001/Kicad_IPAD/releases/assets/<id> > Netloom.ipa`
- Runner kuyruğu yüzünden iptal olan iş: `gh run rerun <id> --failed`.
- `translations.yml`: elle çalıştırılan DeepSeek çeviri incelemesi (`DEEPSEEK_API_KEY` sırrı **yok**, şu an çalışmaz).

## Durum

**main** (`83df3e2`): Faz 6 tamam — şema görüntüleyici, KiCad master kurallarıyla şema netleri,
seçim/vurgu/arama, alt sayfalar, proje modu (klasörde hem kart hem şema varsa geçiş; diğer görünüm
`ParkedView` olarak düzenlemeleriyle bekler). Son düzeltme: `decodeBase64` UB (UBSan buldu).

### Faz 7 — şema düzenleme + "Update PCB from Schematic" (YARIM)

Ajanlarla başlandı, kullanıcı ajanları yarıda kesti. İş üç WIP dalında (hepsi push'lu, hiçbiri
main'e alınmadı, hepsi derleniyor ve ctest geçiyor):

| Dal | Ne | Durum |
|---|---|---|
| `phase7-schedit` (temel `83df3e2`) | `sch_document.*`, `sch_edit.*` (~4200 satır): `kp_sch_doc_*` (çok dosyalı kayıpsız belge, tek undo), `kp_sch_edit_move/rotate/mirror/delete/set_field/set_label_text/add_wire/add_label/add_no_connect/add_junction/route`; `kpcli schedit <kök.kicad_sch> <çıkış-klasörü> ops...` (ops: move, drag, rotate, mirror, delete, field, labeltext, wire, label local/global/hier, junction, note, symbol, sheet, sheetpin, pin) | **Asıl aday.** move/rotate çalışıyor (move KiCad'deki M gibi bağlantıyı koparır — beklenen). **Hata:** `drag R2 2.54 0` R2:2'nin GND bağlantısını kaybediyor (`unconnected-(R2-Pad2)`); büyük olasılıkla pin doğrudan başka pine/güç sembolüne değiyor ve drag bu durumda tel eklemiyor (KiCad ekler). Kontrol: aşağıdaki test. kpcli usage metnine schedit eklenmemiş. Çıkış klasörüne yalnız değişen dosyalar yazılıyor (netlist için alt sayfaları da kopyala). |
| `phase7-schedit-alt` (temel `6fdcc48`) | İlk, daha küçük deneme (~1400 satır) + `tools/sch_edit_roundtrip.cmake` + core.tsv satırları | Yedek. Roundtrip testi ve çeviri satırları `phase7-schedit`'e taşınabilir; geri kalanı büyük olasılıkla atılır. |
| `phase7-sch2pcb` (temel `83df3e2`) | `sch_to_pcb.*` (~1800 satır): `kp_sch_pcb_plan(şema, kart)` → değişiklik listesi (sayılar, satırlar, çevrilmiş metin), `kp_edit_update_from_schematic(doc, kart, şema, ...)`; `kpcli sch2pcb`; core.tsv'ye 27 satır | pic_programmer'da 63/63 footprint yol (path) ile eşleşti, 0 değişiklik (doğru). Değişiklik içeren senaryolar (yeni sembol, silinen sembol, değer/footprint değişimi, net adı değişimi) **test edilmedi**. |

Drag hatasını yeniden üretmek:
```sh
git switch phase7-schedit && cmake --build build
./build/kpcli netlist samples/pic_programmer.kicad_sch > /tmp/n0.txt
./build/kpcli schedit samples/pic_programmer.kicad_sch /tmp/se drag R2 2.54 0
cp samples/pic_sockets.kicad_sch /tmp/se/ && ./build/kpcli netlist /tmp/se/pic_programmer.kicad_sch | diff /tmp/n0.txt -
# beklenen: fark yok (drag bağlantıyı korumalı)
```

Faz 7 yapılacaklar (sırayla öneri):
1. `phase7-schedit`: drag'de pin-pin / pin-güç sembolü temasında tel ekle; her op için
   "düzenle → netlist aynı mı / beklenen fark mı" testi; undo ile bayt bayt geri dönüş (ctest,
   `sch_edit_roundtrip.cmake` alt daldan); KiCad 7 ile açılıp `kicad-cli sch export netlist`
   aynı mı (oracle); ASan korpus taraması.
2. `phase7-sch2pcb`: değişiklikli senaryolar için test şemaları üret, KiCad'in "Update PCB from
   Schematic" sonucuyla karşılaştır (pcbnew Python ile); `main`'e birleştir.
3. Uygulama: şema görünümünde düzenleme modu (taşı/döndür/sil/alan), Pencil ile tel çizme,
   etiket ekleme; proje modunda "Update PCB" düğmesi + değişiklik önizleme sayfası; şema
   dosyalarını klasöre geri kaydetme (`kp_sch_doc_file_text` + `file_modified`).
4. Çeviriler (10 dil), CI ekran görüntüleri (28+), README Faz 7 bölümü, sürüm 0.7.0.

### Çeviri incelemesi (DeepSeek) — beklemede

Çeviriler Claude tarafından yazıldı; ikinci görüş olarak DeepSeek incelemesi istendi, yapılamadı:
GitHub'da `DEEPSEEK_API_KEY` sırrı yok; kullanıcının PC'sindeki OmniRoute'ta (`localhost:20128`,
panel girişi gerekir) deepseek sağlayıcısı 402 (bakiye yok) / 401 verdi, ücretsiz model
(`oc/deepseek-v4-flash-free`) kullanılamadı. Bakiye/anahtar düzelince yerelde (PowerShell):

```powershell
$env:OMNIROUTE_API_KEY = "<OmniRoute panelindeki anahtar>"
python tools\i18n\deepseek_review.py --base-url http://localhost:20128/v1 `
    --key-env OMNIROUTE_API_KEY --model deepseek/deepseek-v4-pro --report review.md
python tools\i18n\generate.py --check
python tools\i18n\generate.py
```
Model kimliği OmniRoute panelinde ne görünüyorsa o (`deepseek/...`, `ds/...`). Betik yalnız
gerekçeli düzeltmeleri yazar; `review.md`'yi gözden geçirip commit et.

## Bilinen eksikler

- KiCad 5 `.sch` (eski biçim) desteklenmiyor; şema KiCad 6–10.
- Şemadaki resimler yer tutucu olarak çiziliyor; yazı tipi KiCad'inkinden farklı (piksel farklarının çoğu bu).
- Etiketlerdeki `${...}` değişkenleri net adlarında çözülmüyor.
- Tek `.kicad_sch` seçilirse yalnız o sayfa görünür (alt sayfalar için klasör açılmalı).
- `.kicad_dru` özel DRC kuralları yok; Gerber'de eski (deprecated) görüntü komutları yok.
- Şema şu an yalnız görüntüleme (Faz 7 bitene kadar).
- Faz 8 (sıfırdan proje: kart kenarı, footprint kütüphanesi, yerleştirme) başlamadı.
