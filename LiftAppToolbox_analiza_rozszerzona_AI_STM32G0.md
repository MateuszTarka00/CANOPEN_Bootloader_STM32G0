# LiftAppToolbox.exe — analiza kodu x86 i wymagania implementacji aktualizacji firmware STM32G0

**Status:** analiza statyczna rzeczywistego pliku wykonywalnego, obejmująca **disassembly fragmentu kodu maszynowego**, rekonstrukcję pseudokodu warstwy PCAN-Basic oraz analizę zasobów. **Nie jest to pełna dekompilacja kreatora firmware.** Nie ma autentycznego trace'u CAN, a protokół aplikacyjny aktualizacji nie został odzyskany bajt po bajcie.

## 1. Źródło, integralność i metody

- Źródło: dostarczony przez użytkownika `LiftAppToolbox.exe`.
- SHA-256: `337f93ffb2b4dcc1ed1a28f96c142602e827cca14f6e4727d623f387f`.
- Typ: natywny PE32, x86, Windows GUI; rozmiar około 31 MiB.
- Sekcje PE: `.text` VA `0x00401000`, rozmiar `0x2BA88E`; `.rdata` VA `0x006BC000`, rozmiar `0x4A6BAC`; `.data`, `.rsrc`, `.reloc`.
- Narzędzia: `file`, `objdump -h`, `objdump -D -Mintel`, `strings`, wyszukiwanie referencji do stałych adresów w bajtach `.text`.
- Ograniczenia: w środowisku nie było Ghidry/IDA/radare2 ani bibliotek Capstone; nie wykonywano aplikacji, nie podłączano urządzenia, nie uzyskano pliku aktualizacyjnego. Dlatego poniższa rekonstrukcja jest **częściowa**.

### Klasyfikacja dowodów

- **[ASM]** — potwierdzone instrukcjami x86 pod konkretnym adresem.
- **[STR]** — potwierdzone tekstem w EXE; sam tekst nie potwierdza, że dana gałąź jest wykonywana.
- **[STD]** — znane zachowanie standardowego CANopen/SDO, niekoniecznie LiftAppToolbox.
- **[DESIGN]** — propozycja implementacji STM32G0, nie wynik dekompilacji.
- **[UNKNOWN]** — nieustalone; wymaga dekompilacji dalszych funkcji lub trace'u.

## 2. Faktyczna rekonstrukcja kodu: adapter PEAK PCAN-Basic

### 2.1 Biblioteka i nazwy funkcji [ASM]

W `.rdata` pod adresem ok. `0x006BEE80` znajduje się napis UTF-16LE `PCANBASIC.DLL`. W tym samym obszarze znajdują się nazwy eksportów ASCII:

| Funkcja | Adres napisu w EXE | Adres instrukcji referującej napis |
|---|---:|---:|
| `CAN_Initialize` | `0x006BEE9C` | `0x00408B55` |
| `CAN_Uninitialize` | `0x006BEEAC` | `0x00408B72` |
| `CAN_Read` | `0x006BEEC0` | `0x00408B89` |
| `CAN_Write` | `0x006BEECC` | `0x00408BA0` |
| `CAN_GetStatus` | `0x006BEED8` | `0x00408BB7` |
| `CAN_SetValue` | `0x006BEEE8` | `0x00408BD1` |

Wskazanie `PCANBASIC.DLL` i wywołania pobierające eksporty potwierdzają, że program używa **dynamicznie ładowanego PCAN-Basic**. Jest to istotne dla loggera: LiftAppToolbox i Python mogą konkurować o inicjalizację tego samego kanału, ale nie jest to dowód na określony mechanizm współdzielenia.

### 2.2 Fragment rzeczywistego disassembly [ASM]

```asm
00408B55  push 0x006BEE9C           ; "CAN_Initialize"
00408B5A  push ebx                  ; uchwyt modułu DLL
00408B61  call dword ptr [0x006BC350] ; GetProcAddress (z kontekstu importów)
00408B67  mov [edi+0x28], eax       ; zachowaj wskaźnik funkcji
00408B6A  test eax, eax
00408B6C  je 0x004089EE            ; brak eksportu -> ścieżka błędu

00408B72  push 0x006BEEAC           ; "CAN_Uninitialize"
00408B78  call dword ptr [0x006BC350]
00408B7E  mov [edi+0x2C], eax

00408B89  push 0x006BEEC0           ; "CAN_Read"
00408B8F  call dword ptr [0x006BC350]
00408B95  mov [edi+0x30], eax

00408BA0  push 0x006BEECC           ; "CAN_Write"
00408BA6  call dword ptr [0x006BC350]
00408BAC  mov [edi+0x34], eax

00408BB7  push 0x006BEED8           ; "CAN_GetStatus"
00408BBD  call dword ptr [0x006BC350]
00408BC3  mov [edi+0xDC], eax

00408BD1  push 0x006BEEE8           ; "CAN_SetValue"
00408BD7  call dword ptr [0x006BC350]
00408BDD  mov [edi+0xD8], eax
```

**Wniosek:** sześć adresów funkcji zapisywanych jest w polach obiektu interfejsu. Wykrycie `CAN_SetValue` **nie dowodzi** używania konkretnego parametru (`PCAN_BITRATE_ADAPTING` ani innego): wymaga analizy miejsc wywołania wskaźnika `[obiekt+0xD8]`.

### 2.3 Pseudokod odzyskany z tego fragmentu [ASM]

```c
// Rekonstrukcja logiki ładowania API, NIE oryginalny kod źródłowy.
bool load_pcan_exports(Adapter *a, HMODULE dll) {
    a->initialize   = GetProcAddress(dll, "CAN_Initialize");
    if (!a->initialize) return false;
    a->uninitialize = GetProcAddress(dll, "CAN_Uninitialize");
    if (!a->uninitialize) return false;
    a->read         = GetProcAddress(dll, "CAN_Read");
    if (!a->read) return false;
    a->write        = GetProcAddress(dll, "CAN_Write");
    if (!a->write) return false;
    a->get_status   = GetProcAddress(dll, "CAN_GetStatus");
    if (!a->get_status) return false;
    a->set_value    = GetProcAddress(dll, "CAN_SetValue");
    if (!a->set_value) return false;
    return true;
}
```

Ta rekonstrukcja dotyczy **warstwy dostępu do CAN**, a **nie algorytmu aktualizacji firmware**.

## 3. Kreator aktualizacji — informacje pochodzące z zasobów [STR]

Znaleziono teksty (wielojęzyczne zasoby):

```text
Node Software Update (Firmware)
Firmware Update/Backup Wizard
1.) Reading of vendor, product and firmware information...
2.) Password input and Bootloader activation...
3.) Firmware transfer and restart of the node...
Select a binary file, that is in fact a proper firmware file for the selected node.
The activation of the bootloader has failed.
The activation of the new firmware has failed.
The firmware update has been finished.
The device does not accept the given firmware.
```

Warianty komunikatów wspominają także o konflikcie Node-ID w trybie bootloadera i możliwości backupu. **Nie znamy liczbowej wartości Node-ID bootloadera.**

W EXE znaleziono ogólne teksty dotyczące `0x6005 'Parameter lock/unlock'`, SDO segmented i block transfer. **Nie udowodniono**, że kreator aktualizacji wykorzystuje te konkretne operacje w podanej kolejności.

### Ostrożnie z innymi aktualizacjami

Program zawiera również teksty o aktualizacji OS/Embedded Linux oraz aktualizacji samego LiftAppToolbox. Nie należy utożsamiać tych ścieżek z `Node Software Update (Firmware)` dla urządzeń CANopen.

## 4. Co nadal pozostaje nieznane [UNKNOWN]

| Pytanie | Stan |
|---|---|
| Dokładny format pliku przyjmowanego przez kreator | Nieustalony; wiadomo jedynie, że kreator opisuje go jako binary file |
| Filtr rozszerzeń i parser nagłówka | Nieustalone |
| Hasło, sposób kodowania i indeks SDO | Nieustalone |
| Czy wykorzystywany jest obiekt `0x6005` | Niepotwierdzone dla kreatora |
| Czy firmware przesyłany jest do `0x1F50:01` | Niepotwierdzone dla LiftAppToolbox |
| Czy narzędzie negocjuje SDO block download | Nieustalone |
| Node-ID bootloadera (`126`?) | Niepotwierdzone |
| Polecenie startu nowej aplikacji | Nieustalone |
| Walidacja obrazu, CRC, podpis i format kontenera | Nieustalone |
| Rzeczywiste ramki CAN i czasy odpowiedzi | Brak autentycznego trace'u |

**Nie wolno generować sztucznego trace'u i nazywać go zapisem LiftAppToolbox.**

## 5. Wymagania projektowe: główna aplikacja STM32G0 [DESIGN]

Poniższe wymagania są propozycją architektury **do dostosowania po poznaniu rzeczywistego protokołu**.

1. Zarezerwuj oddzielne obszary Flash dla bootloadera, aplikacji i trwałych metadanych. Rozmiary i adresy dobierz do dokładnego modelu STM32G0, linker script i rozmiaru stron Flash.
2. Zaimplementuj normalny węzeł CANopenNode (NMT, heartbeat, SDO server, Identity Object `0x1018`).
3. Udostępnij obiekt/komendę wejścia do bootloadera **zgodną z ustalonym później protokołem LiftAppToolbox**; nie zakładaj z góry `0x6005`.
4. Po poprawnym żądaniu zapisz trwały znacznik wejścia do bootloadera (z ochroną integralności), potwierdź SDO, dopiero potem wykonaj kontrolowany reset.
5. Przy starcie bootloader powinien móc odczytać znacznik i zdecydować, czy wejść w tryb aktualizacji, czy uruchomić aplikację.
6. Nie pozwalaj aplikacji nadpisywać obszaru bootloadera.
7. Wydziel interfejs przekazania sterowania: zatrzymanie przerwań, SysTick i peryferiów, ustawienie MSP, VTOR (jeśli wspierany i poprawnie skonfigurowany dla danego STM32G0), skok pod Reset_Handler aplikacji. Zweryfikuj szczegóły względem RM konkretnego układu.

## 6. Wymagania projektowe: bootloader STM32G0 [DESIGN]

### 6.1 Maszyna stanów

```text
BOOT -> CHECK_APP -> RUN_APP
  |         |
  |         +-- invalid image --> BOOTLOADER_WAIT
  +-- update request ----------> BOOTLOADER_WAIT
BOOTLOADER_WAIT -> AUTHENTICATE -> RECEIVE -> VERIFY -> COMMIT -> REBOOT
                       |             |        |
                       +-- ERROR <---+--------+
```

- `BOOTLOADER_WAIT`: uruchom CAN i serwer SDO, z Node-ID ustalonym na podstawie rzeczywistego trace'u/specyfikacji.
- `AUTHENTICATE`: obsłuż dokładny mechanizm żądany przez LiftAppToolbox, po jego ustaleniu.
- `RECEIVE`: zapisuj dane strumieniowo do bufora/Flash, obsługuj SDO transfer z negocjowanym rozmiarem.
- `VERIFY`: zweryfikuj docelowy adres, długość, integralność, kompatybilność hardware i — jeśli projekt wymaga — podpis cyfrowy.
- `COMMIT`: oznacz obraz jako ważny **dopiero po pełnej walidacji**.
- `ERROR`: pozostaw możliwość ponownej aktualizacji po utracie zasilania, timeout lub błędzie SDO.

### 6.2 CANopenNode / SDO

- Rejestruj odpowiednie obiekty OD i callbacki dla zapisów SDO zgodnie z ustalonym protokołem.
- Nie blokuj głównej pętli CANopenNode na czas kasowania Flash bez zaprojektowania timeoutów, kolejkowania i obsługi watchdog.
- Sprawdź, czy wersja CANopenNode obsługuje wymagany tryb SDO block download; nie implementuj block transfer przez proste kopiowanie segmentów.
- Zachowuj poprawne kody SDO abort, obsługę rozmiaru, toggle-bit, CRC bloku (jeśli stosowane), kolejności i powtórzeń.
- Rozdziel walidację **protokołu transportowego SDO** od walidacji **samego obrazu firmware**.

### 6.3 Bezpieczeństwo i odporność

- Nie kasuj jedynej poprawnej aplikacji, zanim nie zapewnisz możliwości odzyskania urządzenia (trwały bootloader).
- Dla aktualizacji in-place zabezpiecz się przed przerwaniem zasilania; dual-slot tylko jeśli rozmiar Flash na to pozwala.
- Sprawdzaj adresy i zakresy Flash, wyrównanie, liczbę bajtów, overflow i zgodność typu urządzenia.
- Nie uruchamiaj obrazu z błędnym MSP/Reset_Handler.
- Rozważ uwierzytelnianie firmware podpisem cyfrowym, jeżeli urządzenie ma wymagania bezpieczeństwa.
- Ogranicz dostęp do funkcji aktualizacji w normalnym trybie pracy dźwigu zgodnie z wymaganiami bezpieczeństwa systemu.

## 7. Jak ustalić prawdziwy protokół: plan reverse engineering

### 7.1 Dalsza dekompilacja EXE

1. Otwórz dokładnie ten EXE (SHA-256 powyżej) w Ghidrze jako `x86:LE:32:default` i wykonaj Auto Analyze.
2. Przejdź do `0x00408B55`; nazwij funkcję `load_pcan_exports`, sprawdź pełne granice funkcji.
3. Wyszukaj referencje do pól wskaźników `+0x28`, `+0x2C`, `+0x30`, `+0x34`, `+0xD8`, `+0xDC`. Zidentyfikuj wszystkie miejsca wywołań `CAN_Write` i `CAN_Read`.
4. Znajdź okno `Firmware Update/Backup Wizard` w zasobach dialogów oraz jego handler (dialog proc/klasa MFC). Prześledź funkcje odpowiadające kolejnym trzem etapom kreatora.
5. Prześledź parser wybranego pliku: `CreateFile`/`ReadFile`, sprawdzenie długości, magic bytes, sumy kontrolnej, danych przekazywanych do SDO.
6. Znajdź miejsca budowania SDO upload/download; odzyskaj **konkretne** indeksy/subindeksy, kolejność żądań, warunki przejść, timeouty i kody błędów.
7. Ustal, czy wartości obiektów pochodzą z EDS/DCF, pliku konfiguracyjnego, profilu CiA 417 czy są zakodowane na stałe.
8. W raporcie zapisz adresy funkcji i zrzuty pseudokodu. Nie wyciągaj wniosków wyłącznie z tekstów UI.

### 7.2 Weryfikacja na CAN

- Rejestruj cały ruch CAN przed otwarciem kreatora, podczas aktywacji bootloadera, transmisji i restartu.
- Zapisuj: timestamp, CAN-ID, DLC, 8 bajtów, kierunek (jeśli dostępny), status błędów.
- Dla aplikacji o Node-ID 22 spodziewane standardowe COB-ID SDO to `0x616` (request) i `0x596` (response), heartbeat `0x716`. Dla innego Node-ID bootloadera identyfikatory przelicz od nowa.
- Dekoduj SDO z rozróżnieniem expedited/segmented/block; zanotuj wszystkie NMT i heartbeat.
- Zapisz bajty pliku firmware i porównaj z payloadem transferu, by ustalić czy narzędzie usuwa nagłówek lub modyfikuje zawartość.
- Wykonuj testy tylko na bezpiecznym stanowisku, nie na pracującym dźwigu.

## 8. Kontrakt implementacyjny dla AI

**Zadanie:** przygotuj dwa projekty firmware (`application` i `bootloader`) na wskazany dokładnie STM32G0, z CANopenNode, zgodnie z ustalonymi powyżej wymaganiami bezpieczeństwa. Oddziel część standardową od adaptera protokołu LiftAppToolbox.

**Zanim wygenerujesz kod kompatybilności:** poproś o rzeczywisty plik aktualizacyjny lub trace CAN albo wynik dekompilacji funkcji kreatora. Nie wpisuj arbitralnie Node-ID `126`, hasła `0x6005`, transferu `0x1F50:01` ani formatu `.bin` jako faktów o LiftAppToolbox.

**Docelowe artefakty:**

- `bootloader/`: inicjalizacja HAL/FDCAN (lub bxCAN zależnie od układu), CANopenNode, OD, SDO download, Flash writer, CRC/weryfikacja, state machine, jump to application.
- `application/`: OD i handler przejścia do bootloadera, identyfikacja, heartbeat, NMT, kontrolowany reset.
- `common/`: mapy pamięci, manifest obrazu, trwały znacznik aktualizacji, API walidacji, wspólne definicje protokołu.
- Testy jednostkowe parsera firmware i zapisu Flash; testy integracyjne na magistrali CAN; scenariusze przerwania zasilania, błędów transferu i konfliktu Node-ID.
- Macierz zgodności `funkcja -> adres x86 lub trace -> implementacja C -> test`.

## 9. Podsumowanie stopnia poznania

**Potwierdzono w kodzie x86:** dynamiczne pobieranie sześciu eksportów `PCANBASIC.DLL` i zapis ich wskaźników w obiekcie adaptera.

**Potwierdzono w zasobach:** kreator aktualizacji CiA 417 z etapami identyfikacji, hasła/aktywacji bootloadera, transferu i restartu; wykrywanie błędów i konfliktów Node-ID.

**Nie odzyskano:** wewnętrznej funkcji kreatora budującej SDO, formatu firmware, hasła, Node-ID bootloadera i dokładnego trace'u. Do tego konieczna jest dalsza dekompilacja i/lub pomiar na CAN.

**Uwaga dla kolejnego AI:** nie przedstawiaj projektu architektury z sekcji 5–6 jako odtworzonego zachowania LiftAppToolbox. Pseudokod z sekcji 2.3 jest rekonstrukcją rzeczywiście zidentyfikowanego fragmentu warstwy PCAN, nie kodem aktualizacji.


## 10. Uzupełnienie analizy — zakres możliwy do zweryfikowania bez uruchomienia aplikacji

**Stan analizy:** to nadal *częściowa* dekompilacja. Samo występowanie nazw obiektów lub komunikatów w zasobach nie dowodzi sekwencji wysyłanych ramek. Nie udało się wiarygodnie wyznaczyć pełnego grafu wywołań kreatora aktualizacji, parsera pliku ani algorytmu przesyłu. Nie wolno przedstawiać tego dokumentu jako kompletnej specyfikacji protokołu LiftAppToolbox.

### 10.1 Dodatkowe ciągi wykryte w pliku EXE [STR]

Poniższe napisy występują w obrazie programu (część w zasobach wielojęzycznych):

```text
Firmware Update/Backup Wizard
Node Software Update (Firmware)
1.) Auslesen von Hersteller, Produkt und Firmwareversion...
2.) Passworteingabe und Aktivierung des Bootloaders...
3.) Firmwaretransfer und Neustart des Knotens...
Die Bootloader Aktivierung ist fehlgeschlagen.
Die Aktivierung der neuen Firmware ist fehlgeschlagen.
Das Firmwareupdate wurde abgeschlossen.
Die angegebene Datei ist leer (0 Bytes lang).
```

**Interpretacja:** kreator ma etapy odczytu identyfikacji, hasła i aktywacji bootloadera, przesyłu, restartu oraz walidacji błędów. Nie wiadomo, czy każda z tych ścieżek jest aktywna w konkretnej wersji i dla konkretnego urządzenia.

### 10.2 Co musi zostać odzyskane z kodu x86 [UNKNOWN]

1. Funkcja wywołana przez przycisk rozpoczęcia aktualizacji; adres funkcji, jej argumenty i przepływ sterowania.
2. Parser wejściowego firmware: filtry rozszerzeń, nagłówek, odczyt długości, ewentualne CRC, podpis, identyfikatory producenta/produktu.
3. Odczyty SDO poprzedzające transfer: dokładne indeksy, subindeksy i oczekiwane wartości.
4. Hasło/odblokowanie: indeks/subindeks, sposób kodowania i źródło hasła; nie wpisywać hipotetycznego hasła na stałe.
5. Komenda aktywacji bootloadera i sposób przejścia na jego Node-ID.
6. Transfer danych: segmented/block, inicjalizacja, wielkość bloku, CRC, potwierdzenia, retransmisje i timeouty.
7. Komendy aktywacji nowego obrazu, weryfikacja i reset.
8. Obsługa aktualizacji przerwanej przez utratę zasilania.

### 10.3 Ścieżka do pełniejszej dekompilacji

Zaimportować oryginalny PE32 do Ghidry, wykonać Auto Analysis i znaleźć odwołania do komunikatów kreatora oraz kodu ładującego PCAN-Basic. Następnie przejść od obsługi zdarzenia „Firmware Update starten” do funkcji przygotowującej SDO, odszukać wszystkie wywołania warstwy CAN, a dla każdej funkcji zapisać adres, pseudokod, stałe i dowody. Dekompilować także funkcje wywoływane pośrednio, nie tylko te z bezpośrednimi referencjami do napisów. Uzupełnić wyniki o prawdziwy trace CAN i plik wejściowy zaakceptowany przez program.

### 10.4 Wymagania dla AI implementującego STM32G0

- **Aplikacja główna:** identyfikacja CANopen, kontrolowane przejście do bootloadera, trwały znacznik żądania aktualizacji, bezpieczny reset; szczegóły obiektów ustalić na podstawie kodu lub trace'u.
- **Bootloader:** niezależny stos CANopen/SDO, walidacja docelowego produktu, zapis Flash z kontrolą zakresów, integralność obrazu, atomowy znacznik ważności, możliwość ponowienia aktualizacji po awarii zasilania, skok do aplikacji dopiero po walidacji.
- **Kontrakt kompatybilności:** nie uznawać implementacji za zgodną z LiftAppToolbox, dopóki nie przejdzie testu rzeczywistej aktualizacji i porównania ramek CAN.

### 10.5 Ważne ostrzeżenie dotyczące adresów i standardu

Adresy `0x00408B55` itd. wskazane w sekcji 2 to miejsca odwołujące się do nazw eksportów PCAN-Basic, **nie adresy procedur firmware update**. Obiekty `0x6005`, `0x1F50` i Node-ID 126 są punktem odniesienia CANopen-Lift, a nie niezależnie zweryfikowanym śladem konkretnej aktualizacji w LiftAppToolbox.
