# samsung-m2020-printer-app

Driver moderno ("Printer Application") per le stampanti laser **Samsung M2020 / M2021 / M2022 / M2026** collegate via USB.
Gira su **macOS 27+ (Apple Silicon)** e su **Raspberry Pi 4/5 con Ubuntu Server arm64**.

Non installa nessun driver nel sistema: espone la stampante come stampante **IPP Everywhere / AirPrint**, quindi Mac, iPhone, iPad, Linux e Windows ci stampano senza driver.

```
Mac / iPhone / PC ──IPP/AirPrint──▶ samsung-m2020-printer-app ──USB──▶ Samsung M2026
                                     │
                                     ├─ PAPPL: riceve il lavoro, lo rasterizza e lo retina a 1 bit
                                     └─ SpliX rastertoqpdl + m2020.ppd: converte in QPDL (il linguaggio della stampante)
```

Il linguaggio della stampante (QPDL v3, compressione 0x11) è generato da [SpliX](https://github.com/OpenPrinting/splix), che supporta ufficialmente la "M2020 Series" dalla versione 2.0.2. Questa app è il "collante" moderno tra SpliX e IPP.

## Stato

| Cosa | Stato |
|---|---|
| Compilazione e catena PAPPL → SpliX → QPDL | ✅ testata (Linux, output su file) |
| Formati in ingresso PWG raster e PNG | ✅ testati (URF e JPEG passano dallo stesso percorso PAPPL, non testati qui) |
| Copie, A4/A5/Letter…, modalità bozza (toner save), densità toner | ✅ verificati nel flusso PJL/QPDL generato |
| Stampa reale su M2026 via USB | ⏳ **da provare sul tuo hardware** |
| Build su macOS 27 arm64 | ⏳ da provare (script pronto) |

## 1. Compilazione

Serve una sola volta. Lo script scarica e compila (versioni fissate e testate) libcups 2.4, PAPPL 1.4.9 e SpliX dentro `build/deps`, senza toccare il sistema.

### macOS 27 (Apple Silicon)

```sh
xcode-select --install          # se non hai già i Command Line Tools
# Homebrew è richiesto: https://brew.sh
scripts/build-deps.sh           # installa pkgconf libusb libpng jpeg-turbo openssl@3 via brew
make DEPS=$PWD/build/deps
sudo make install DEPS=$PWD/build/deps
```

### Raspberry Pi 4/5 — Ubuntu Server arm64

```sh
sudo apt install -y avahi-daemon   # serve per farla trovare in rete (Bonjour)
scripts/build-deps.sh              # installa i pacchetti -dev via apt
make DEPS=$PWD/build/deps
sudo make install DEPS=$PWD/build/deps
```

Vengono installati:

- `/usr/local/bin/samsung-m2020-printer-app`
- `/usr/local/libexec/samsung-m2020-printer-app/rastertoqpdl` (SpliX)
- `/usr/local/share/samsung-m2020-printer-app/m2020.ppd` (generato da SpliX)

## 2. Avvio come servizio

### Raspberry Pi (systemd)

```sh
sudo cp packaging/linux/samsung-m2020-printer-app.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now samsung-m2020-printer-app
journalctl -u samsung-m2020-printer-app -f        # log
```

Gira come root (accesso USB semplice; stacca da solo il modulo kernel `usblp`). Lo stato è salvato in `/var/lib/samsung-m2020-printer-app.state`.

### macOS (LaunchAgent utente)

Prima **elimina eventuali code esistenti della M2026** in Impostazioni di Sistema → Stampanti e Scanner, così il CUPS di macOS non contende la porta USB.

```sh
scripts/install-macos-agent.sh
```

Parte al login, ascolta sulla porta 8000, log in `~/Library/Logs/samsung-m2020-printer-app.log`.

## 3. Aggiungere la stampante

Con la stampante accesa e collegata via USB, al primo avvio l'app la **aggiunge da sola** (riconosce il device ID USB "Samsung M2020 Series"). Altrimenti:

```sh
samsung-m2020-printer-app devices                     # trova l'URI usb://...
samsung-m2020-printer-app add -d M2026 -m samsung_m2020 -v 'usb://Samsung/M2020%20Series?serial=...'
```

Puoi gestire tutto anche dall'interfaccia web: `http://localhost:8000` sul Mac, oppure `http://<ip-del-pi>:8000` (dal Pi stesso; per amministrarla da un altro computer aggiungi `-o server-options=web-remote` nel servizio, sapendo che chiunque in LAN potrà modificarla).

Poi sul Mac: **Impostazioni di Sistema → Stampanti e Scanner → Aggiungi**: compare come stampante AirPrint/Bonjour. Se non compare, aggiungila a mano dalla scheda IP: indirizzo `localhost:8000` (o `<ip-del-pi>:8000`), protocollo IPP, coda `ipp/print/M2026`, "Usa: AirPrint/Secure AirPrint".

Prova rapida da riga di comando:

```sh
samsung-m2020-printer-app submit -d M2026 qualcosa.png
samsung-m2020-printer-app jobs -d M2026 -a
```

## Opzioni supportate

| IPP (quello che scegli nel dialogo di stampa) | Cosa diventa per SpliX / la stampante |
|---|---|
| Formato carta (A4, A5, A6, Letter, Legal, Executive, Folio, B5, B6, buste…) | `PageSize` |
| Alimentazione: automatica / manuale | `InputSlot=Auto/Manual` |
| Tipo carta (normale, sottile, spessa, cartoncino, etichette, buste…) | `MediaType` |
| Qualità bozza | `EconoMode=ON` (risparmio toner) |
| "printer-darkness-configured" (pagina web della stampante) | `TonerDensity` chiaro / medio / scuro |
| Risoluzione | 600 dpi (1200 dpi abilitabile con `M2020_ENABLE_1200DPI=1`) |
| Copie | gestite da PAPPL |

Solo fronte (la M2020 non ha duplex; il duplex manuale di SpliX non è esposto).

## Risoluzione problemi

- **Log dettagliato**: avvia con `-o log-level=debug`. I messaggi di SpliX compaiono con prefisso `SpliX:`.
- **Pagine bianche o spazzatura**: avvia con la variabile `M2020_KEEP_RASTER=1`; il raster intermedio resta in `$TMPDIR` (percorso nel log) per l'analisi. Prova anche senza 1200 dpi.
- **Stampa raw QPDL** (per test): l'app accetta file `application/vnd.samsung-qpdl` e li manda così come sono alla stampante.
- **"server-error-busy"** su un secondo lavoro: è normale con PAPPL; i client IPP (macOS incluso) ritentano da soli quando la stampante si libera.
- **macOS non la trova via Bonjour**: controlla che il processo abbia il permesso "Rete locale" (Impostazioni di Sistema → Privacy e sicurezza) oppure aggiungila a mano come IPP (vedi sopra).
- Percorsi alternativi del filtro e del PPD: variabili `M2020_FILTER` e `M2020_PPD`.

## Licenze

- Codice di questa app: Apache-2.0.
- PAPPL: Apache-2.0. libcups: Apache-2.0.
- SpliX (`rastertoqpdl`) e `ppd/m2020.ppd` (generato dai sorgenti SpliX): GPL-2.0. Il filtro è eseguito come programma separato.
