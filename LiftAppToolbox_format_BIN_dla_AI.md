# LiftAppToolbox — format pliku firmware `.bin` (notatka dla AI)

## Cel i status wiedzy

Celem jest implementacja aktualizacji aplikacji STM32G0 z CANopenNode przez LiftAppToolbox. **Nie ustalono dokładnego, wymaganego przez LiftAppToolbox formatu pliku `.bin`.** Poniższy 64-bajtowy nagłówek jest **propozycją własnego formatu**, a nie wynikiem potwierdzonej dekompilacji ani specyfikacją LiftAppToolbox. Nie wolno zakładać, że LiftAppToolbox zaakceptuje taki plik.

## Co wiadomo o LiftAppToolbox

W dotychczasowej analizie przesłanego `LiftAppToolbox.exe` odnotowano komunikat kreatora: „Select a binary file, that is in fact a proper firmware file for the selected node.” Wskazuje on, że narzędzie oczekuje binarnego pliku odpowiedniego dla wybranego węzła, **ale nie potwierdza**, czy jest to surowy obraz `.bin`, kontener z nagłówkiem, plik podpisany ani jakie rozszerzenia są dopuszczane. CANopen/SDO definiuje transmisję danych, nie format bajtowy obrazu firmware.

Niewyjaśnione pozostają: czy LiftAppToolbox przesyła cały plik 1:1, czy usuwa nagłówek; czy weryfikuje Vendor ID/Product Code; czy CRC jest w pliku; czy sprawdza adres aplikacji; czy wymaga konkretnego formatu podpisu; jakiego filtra rozszerzeń używa okno wyboru pliku.

## Proponowany *własny* kontener firmware (niepotwierdzony dla LiftAppToolbox)

```
Offset 0x0000 .. 0x003F: nagłówek 64 B
Offset 0x0040 .. końca: obraz aplikacji STM32 (surowe bajty)
```

| Offset | Długość | Pole | Znaczenie / przykład |
|---|---:|---|---|
| `0x00` | 4 | Magic | ASCII `FWUP` |
| `0x04` | 2 | Format version | `1` |
| `0x06` | 2 | Header size | `64` |
| `0x08` | 4 | Vendor ID | identyfikator producenta |
| `0x0C` | 4 | Product Code | identyfikator produktu |
| `0x10` | 4 | Firmware version | np. `0x00010002` |
| `0x14` | 4 | Application address | np. `0x08008000` (tylko przykład) |
| `0x18` | 4 | Firmware size | liczba bajtów samego obrazu |
| `0x1C` | 4 | Firmware CRC32 | CRC obrazu (parametry CRC wymagają zdefiniowania) |
| `0x20` | 4 | Header CRC32 | CRC nagłówka (należy ustalić zakres i wyzerowanie pola CRC) |
| `0x24` | 28 | Reserved | zera |
| `0x40` | N | Application binary | obraz aplikacji z tablicą wektorów na początku |

Wszystkie pola liczbowe wielobajtowe: **little-endian**. To projekt, który można implementować w *własnym* narzędziu i bootloaderze; nie traktować go jako wymogu LiftAppToolbox.

## Walidacja w bootloaderze (zalecany projekt)

1. Zweryfikować magic, wersję formatu, długość nagłówka i CRC nagłówka (po precyzyjnym ustaleniu algorytmu).
2. Zweryfikować zgodność Vendor ID / Product Code i politykę wersji firmware.
3. Zweryfikować adres docelowy, długość obrazu, zakres pamięci Flash, wyrównanie stron i to, że nie nadpisuje bootloadera.
4. Odebrać obraz i zapisać go w bezpieczny sposób; nie oznaczać aplikacji jako prawidłowej przed kompletnym zapisem.
5. Zweryfikować CRC obrazu i tablicę wektorów (SP i adres resetu) oraz dopiero wtedy zezwolić na start aplikacji.
6. Zapewnić bezpieczne zachowanie po zaniku zasilania w czasie aktualizacji; rozważyć podpis kryptograficzny, jeśli wymagana jest autentyczność firmware (CRC nie chroni przed celową modyfikacją).

**Uwaga:** podany `0x08008000` to wyłącznie przykładowy adres; prawidłowy zależy od konkretnego STM32G0, mapy Flash, linkera i wielkości bootloadera.

## Rekomendowany plan zgodności z LiftAppToolbox

1. Zbudować **`application.bin`** jako surowy obraz aplikacji (np. z STM32CubeIDE/objcopy), z tablicą wektorów na początku. **Nie dodawać automatycznie nagłówka `FWUP`**.
2. Osobno utworzyć **`application_fwup.bin`** z opisanym nagłówkiem, tylko do testów własnego formatu.
3. Sprawdzić w LiftAppToolbox filtr rozszerzeń i zachowanie po wybraniu pliku; nie przeprowadzać testowego zapisu na urządzeniu bez planu odzyskiwania.
4. Pozyskać prawdziwy plik aktualizacyjny zaakceptowany przez LiftAppToolbox oraz trace CAN z rzeczywistej sesji; ustalić, czy transfer SDO przenosi plik 1:1 i jakie są wymagane komendy sterujące.
5. Dopiero na podstawie tych dowodów dostosować parser, obsługę SDO i procedurę startu bootloadera w aplikacji STM32G0.

## Instrukcja dla AI implementującego

**Nie przedstawiaj proponowanego kontenera `FWUP` jako formatu LiftAppToolbox.** Wyraźnie oddzielaj: (a) informacje potwierdzone w EXE, (b) standard CANopen/CiA-417, (c) propozycję architektury własnego bootloadera, (d) hipotezy do sprawdzenia. Nie generuj fikcyjnego trace'u jako autentycznego. Jeśli celem jest kompatybilność z LiftAppToolbox, zażądaj rzeczywistego pliku aktualizacji i/lub zarejestrowanej komunikacji CAN.
