# Aktualizacja firmware noda CANopen przez LiftAppTool — specyfikacja robocza dla AI

## 0. Status i ograniczenia wiedzy

Ten dokument opisuje **model referencyjny** aktualizacji firmware przez CANopen, przydatny do analizy i implementacji bootloadera STM32G0 współpracującego z LiftAppTool. **Nie jest potwierdzonym zapisem działania konkretnej wersji LiftAppTool**. Bez pliku EDS, dokumentacji narzędzia i logu CAN nie można stwierdzić, że LiftAppTool używa dokładnie wszystkich poniższych obiektów, subindeksów, wartości poleceń i sekwencji.

**Wymóg dla AI:** Rozróżniaj: (A) reguły protokołu CANopen SDO/NMT, (B) typowy model programowania wg CiA 302-3, (C) założenia projektowe dla konkretnego bootloadera. Nie traktuj przykładów ramek jako zarejestrowanych komunikatów LiftAppTool.

## 1. Cel

Zrozumieć i odtworzyć aktualizację aplikacji urządzenia CANopen przez LiftAppTool, w tym:
- identyfikację noda;
- wejście do bootloadera;
- zatrzymanie / przygotowanie programu;
- przesłanie firmware przez SDO;
- walidację obrazu;
- uruchomienie aplikacji i potwierdzenie wersji.

Kontekst projektu: STM32G0, CANopenNode, aplikacja związana z CiA 417, własny bootloader z manifestem i podpisem Ed25519. Samo posiadanie podpisanej paczki ZIP nie oznacza, że LiftAppTool umie ją odczytać lub przesłać.

## 2. Identyfikatory CAN dla przykładowego Node-ID 22 (0x16)

| Rodzaj | COB-ID / CAN-ID | Uwagi |
|---|---|---|
| SDO client -> server (żądanie) | 0x616 | 0x600 + 0x16, domyślne SDO |
| SDO server -> client (odpowiedź) | 0x596 | 0x580 + 0x16, domyślne SDO |
| Heartbeat / boot-up | 0x716 | 0x700 + 0x16 |
| NMT | 0x000 | 2 bajty danych: polecenie, Node-ID |

Uwaga: powyższe SDO COB-ID to ustawienia domyślne. Urządzenie może mieć inne skonfigurowane identyfikatory.

## 3. Obiekty słownika, które należy sprawdzić

| Indeks | Znaczenie | Status |
|---|---|---|
| 0x1000 | Device Type | standardowy obiekt identyfikacyjny |
| 0x1001 | Error Register | standardowy |
| 0x100A | Manufacturer Software Version | opcjonalny, zależny od urządzenia |
| 0x1018:01..04 | Vendor ID, Product Code, Revision, Serial | Identity Object |
| 0x1F50 | Program Data | mechanizm programowania CANopen; sprawdzić subindeksy |
| 0x1F51 | Program Control | mechanizm programowania CANopen; sprawdzić subindeksy i dopuszczalne wartości |
| 0x1F56 | Program Software Identification | sprawdzić dostępność i format |
| 0x1F57 | Flash Status Identification | sprawdzić dostępność, format i znaczenie statusów |
| 0x200A | Obiekt producenta | znaczenie nieznane bez EDS/kodu źródłowego |

Wartość `0xFE0001A1` dla `0x1000` w trybie bootloadera była omawiana jako możliwy identyfikator w kontekście CiA 417; **należy ją potwierdzić w dokumentacji i implementacji**. Nie zakładaj jej bez sprawdzenia.

## 4. Sekwencja logiczna aktualizacji

### Etap 1: Wykrycie i identyfikacja
1. LiftAppTool wykrywa noda na magistrali CAN.
2. Może odczytać przez SDO obiekty `0x1000`, `0x1018`, `0x100A` i/lub inne informacje o oprogramowaniu.
3. Sprawdza kompatybilność urządzenia z firmware i możliwość aktualizacji.

Przykład odczytu `0x1018:01` (Vendor ID):

```text
TX 0x616 [8] 40 18 10 01 00 00 00 00
RX 0x596 [8] 43 18 10 01 VV VV VV VV
```

`VV VV VV VV` to wartość UINT32 w little-endian; `43` jest przykładową odpowiedzią expedited o długości 4 bajtów.

### Etap 2: Przejście do bootloadera
1. Narzędzie może przełączyć urządzenie do NMT Pre-operational, zatrzymać aplikację albo użyć mechanizmu producenta.
2. Przykład NMT Enter Pre-operational dla noda 22:

```text
TX 0x000 [2] 80 16
```

3. **NMT Pre-operational samo w sobie nie uruchamia bootloadera.** Potrzebny jest konkretny mechanizm przejścia zaimplementowany w urządzeniu (np. obiekt OD, polecenie aktualizacji, reset z flagą w pamięci).
4. Po przejściu do bootloadera należy potwierdzić, że odpowiada na SDO.

### Etap 3: Przygotowanie programowania
1. Klient może użyć obiektu `0x1F51` do sterowania programem.
2. Typowe pojęcia: stop, start, reset, clear/erase — **dokładne wartości poleceń i dozwolone stany należy sprawdzić w normie, EDS i implementacji**.
3. Bootloader musi zabezpieczyć przed przypadkowym nadpisaniem własnego kodu i obszarów NVM.
4. Nie wysyłaj poleceń kasowania na fizyczne urządzenie tylko na podstawie przykładowych ramek.

### Etap 4: Transfer firmware
1. Klient rozpoczyna SDO Download do obiektu danych programu, zwykle `0x1F50` (często subindeks `0x01`, ale wymaga weryfikacji).
2. Firmware może być przesyłane jako SDO segmented download lub SDO block download; konkretny tryb zależy od klienta i serwera.
3. W segmented SDO dane są dzielone na segmenty zawierające maksymalnie 7 bajtów danych użytkownika, z bitem toggle i potwierdzeniami.
4. W block SDO transfer jest grupowany w bloki z innym schematem potwierdzeń; implementacja musi obsługiwać właściwy protokół, jeśli używa go LiftAppTool.

Przykład **hipotetycznej** inicjalizacji segmented download do `0x1F50:01` dla 4096 bajtów:

```text
TX 0x616 [8] 21 50 1F 01 00 10 00 00
RX 0x596 [8] 60 50 1F 01 00 00 00 00
```

`0x21` oznacza inicjalizację download z podanym rozmiarem; `00 10 00 00` = 4096 little-endian. To ilustracja CANopen SDO, **nie potwierdzona ramka LiftAppTool**.

Po inicjalizacji kolejne ramki SDO segmented nie zawierają już indeksu `0x1F50`; analizator musi śledzić sesję i toggle.

### Etap 5: Zakończenie i walidacja
1. Bootloader potwierdza poprawne zakończenie transferu na poziomie SDO.
2. Sprawdza długość, zakres adresów, integralność i format obrazu.
3. W projekcie z Ed25519 dodatkowo weryfikuje podpis, manifest, zgodność sprzętową, wersję i politykę anty-rollback, jeśli jest zaimplementowana.
4. Może raportować status przez `0x1F57` lub własne obiekty. Dokładne subindeksy i kody statusu wymagają potwierdzenia.
5. **Potwierdzenie SDO nie jest automatycznie potwierdzeniem poprawnego podpisu ani uruchamialności firmware.**

### Etap 6: Aktywacja aplikacji
1. Po pomyślnej walidacji klient może wydać polecenie uruchomienia programu lub reset noda.
2. Bootloader przełącza się na zweryfikowaną aplikację zgodnie z własnym mechanizmem (np. flaga aktywnego slotu + reset).
3. LiftAppTool ponownie wykrywa noda i weryfikuje wersję / tożsamość oprogramowania.

## 5. Znaczenie podstawowych ramek

```text
40 II JJ SS 00 00 00 00   SDO upload initiate: odczyt indeksu 0xJJII, subindeks SS
2F II JJ SS DD 00 00 00   SDO expedited download: zapis 1 bajtu DD
2B II JJ SS DD DD 00 00   SDO expedited download: zapis 2 bajtów
23 II JJ SS DD DD DD DD   SDO expedited download: zapis 4 bajtów
60 II JJ SS 00 00 00 00   SDO potwierdzenie initiate download
80 II JJ SS EE EE EE EE   SDO Abort; kod błędu EE.. w little-endian
```

Odpowiedź odczytu expedited może zaczynać się np. `4F` (1 bajt), `4B` (2 bajty), `47` (3 bajty) albo `43` (4 bajty). Dla większych danych używa się transferu segmentowego lub blokowego.

## 6. Mapa obowiązków: LiftAppTool kontra bootloader

| LiftAppTool / klient | Bootloader / serwer SDO |
|---|---|
| Wykrywa Node-ID i czyta OD | Udostępnia identyfikację i status |
| Inicjuje wejście w tryb aktualizacji | Zapewnia kontrolowane przejście z aplikacji |
| Wysyła polecenia sterujące | Waliduje stan i autoryzację poleceń |
| Wysyła dane przez SDO | Odbiera, buforuje i zapisuje obraz bezpiecznie |
| Odczytuje status | Weryfikuje obraz i raportuje wynik |
| Inicjuje start/reset | Uruchamia tylko poprawny, zaakceptowany obraz |

## 7. Specyfika istniejącego projektu STM32G0

Z wcześniejszych informacji projekt wykorzystuje:
- bootloader zajmujący początkowe 36 KiB Flash;
- początek aplikacji Slot A: `0x08009000`;
- własny manifest i podpis Ed25519;
- plik aktualizacji ZIP zawierający `firmware.bin`, `manifest.bin`, `package.json`.

**Pytanie otwarte:** Czy LiftAppTool przesyła surowy BIN, ZIP, czy inny format? Nie należy zakładać, że rozumie manifest i ZIP projektu. Możliwe jest wymaganie adaptera/narzędzia pośredniego albo osobnego protokołu aktualizacji. Rozwiązanie musi zachować sprawdzanie podpisu w bootloaderze.

## 8. Co należy zebrać, żeby odtworzyć faktyczny algorytm LiftAppTool

1. Wersja LiftAppTool i model aktualizowanego urządzenia.
2. EDS/DCF noda oraz definicje `0x1F50`, `0x1F51`, `0x1F56`, `0x1F57` i ewentualnie `0x200A`.
3. Zrzut CAN od chwili wykrycia noda do ponownego uruchomienia (timestamp, CAN-ID, DLC, DATA, kierunek).
4. Plik firmware wybrany w LiftAppTool i jego format.
5. Reakcje na błędy, przerwanie zasilania i nieudany podpis.
6. Kod źródłowy implementacji OD i obsługi SDO w bootloaderze.

Uwaga operacyjna: PCAN-View może nie móc jednocześnie otworzyć tego samego kanału PCAN-USB co LiftAppTool. W razie konfliktu użyj osobnego interfejsu CAN pracującego jako pasywny sniffer.

## 9. Instrukcja dla kolejnego modelu AI

Pracuj na tym dokumencie jako na **hipotezie implementacyjnej**, nie jako na dowodzie działania LiftAppTool. Najpierw ustal fakty na podstawie EDS, źródeł i logów CAN. Oznaczaj każdą ramkę jako: `POTWIERDZONA_Z_LOGU`, `STANDARD_CANOPEN`, `PRZYKŁAD`, `NIEZWERYFIKOWANA`. Nie zgaduj subindeksów, poleceń kasowania, wartości statusów ani formatu firmware. Przy analizie logu rekonstruuj sesję SDO, zwłaszcza transfery segmented/block, zamiast wyszukiwać `0x1F50` w każdej ramce. Dla operacji zapisu Flash wymagaj jawnej weryfikacji mapy pamięci i mechanizmów bezpieczeństwa.
