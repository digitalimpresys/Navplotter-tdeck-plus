# Plotter GPS nautico per LilyGO T-Deck Plus

Porting del progetto `plotter` (ESP32 CYD) sulla T-Deck Plus. Il progetto CYD originale non è stato modificato.

## Compilare e caricare

```
~/.platformio/penv/bin/pio run                 # compila
~/.platformio/penv/bin/pio run -t upload       # carica (via USB-C)
~/.platformio/penv/bin/pio device monitor      # seriale a 115200
```

Se il caricamento non si collega: tieni premuta la trackball (è il tasto BOOT), premi e rilascia RESET, poi rilascia la trackball.

## Cosa cambia rispetto alla CYD

- **Display 320x240 orizzontale**, disegnato prima in memoria e poi copiato in un colpo: niente sfarfallio. Si aggiorna ogni secondo.
- **Touch capacitivo GT911**: niente più calibrazione all'avvio.
- **Tocco lungo** (circa 1 secondo, dito fermo) sulla mappa: aggiunge un waypoint in quel punto.
- **Trackball**: sulla mappa la sposta; premuta centra sulla barca. Nelle liste sceglie la voce; premuta su un waypoint lo mostra sulla mappa.
- **GPS integrato**: velocità (38400 / 9600 / 115200) rilevata da sola.
- **Al primo fix** la mappa si centra sulla barca, se non c'è una configurazione salvata.
- **Distanze in miglia nautiche (NM)**.
- **Sidebar**: M mappa, W waypoint, R rotte, S impostazioni, B bussola, + e - zoom. È sempre visibile, uguale su tutte le pagine; i comandi propri di una pagina stanno in una riga in basso e ogni pulsante riporta il tasto equivalente (per esempio "g Vai").
- **Destinazione ("vai a")**: nella lista waypoint, tasto `g` o pulsante "Vai". Sulla mappa compare una linea rossa dalla barca al waypoint, e nella barra di stato rilevamento e distanza. Non viene salvata: al riavvio va reimpostata.
- **Pagina bussola**: rosa con la prora in alto, punti cardinali, freccia verso la destinazione, rilevamento, distanza e gradi di accostata. La prora è la rotta GPS (la scheda non ha bussola magnetica): sotto 0,5 nodi resta l'ultimo valore valido.
- **SD**: stessi file della CYD (`config.txt`, `waypoints.txt`, `routes.txt`), quindi la stessa scheda funziona su entrambi.
- Le impostazioni mostrano se GPS, SD, touch e tastiera sono stati trovati.
- **Mappe a tile da SD**: `/tiles/z/x/y.jpg` (schema XYZ, Web Mercator, JPEG 256x256). Lo zoom va a livelli interi, limitati a quelli presenti sulla scheda; dove manca una tile resta lo sfondo azzurro.

## Tastiera

| Tasto | Azione |
|---|---|
| m / w / r / s | mappa / waypoint / rotte / impostazioni |
| b | pagina bussola |
| g | nella lista waypoint: imposta (o annulla) la destinazione |
| i o + / o o - | zoom + / zoom - |
| c | centra sulla barca e la segue (la barca resta al centro); spostando la mappa con la trackball si smette di seguirla |
| a | waypoint sulla posizione della barca |
| e | rinomina il waypoint o la rotta selezionati (nelle liste) |
| d | elimina il waypoint o la rotta selezionati (nelle liste) |
| n / k | nella lista rotte: nuova rotta / riepilogo della rotta |
| Enter / Backspace | conferma / cancella durante la scrittura del nome |

## Bug corretti rispetto alla versione CYD

- Rotta vuota: il disegno leggeva fuori dalla lista dei waypoint.
- Tocco lungo: scattava a caso; ora scatta solo con il dito fermo per circa un secondo.
- ID waypoint duplicati dopo un'eliminazione.
- Waypoint aggiunti con il tocco non salvati su SD.
- Pulsanti "Torna alla mappa" e "Calibra" invertiti nel menu impostazioni.
- La sidebar veniva cancellata a ogni aggiornamento della mappa.
- Centro mappa salvato con precisione ridotta (float invece di double).

## Traffico LoRa (ramo `lora-traffico`, in prova)

I plotter con questo firmware si scambiano via radio nome, posizione, rotta e velocità, e si vedono a vicenda sulla mappa. Non è AIS: si vedono solo le barche con questo stesso firmware.

- Radio: 869,525 MHz, 125 kHz, SF9, 22 dBm (banda europea 869,4-869,65 MHz, 10% del tempo). La frequenza si cambia con `loraFreq=` in `config.txt`.
- Un pacchetto ogni 10 secondi in movimento e ogni 30 da fermi, solo con il fix GPS.
- Le altre barche compaiono sulla mappa come triangoli blu con nome e vettore di rotta; grigie dopo un minuto senza notizie, tolte dopo tre.
- Allarme di abbordo: se il massimo avvicinamento previsto (CPA) è sotto 0,1 miglia entro 5 minuti, la barca diventa rossa e compare una fascia rossa in alto.
- Tasto `t` (o "Barche vicine" nelle impostazioni): elenco con distanza, rilevamento, CPA e segnale.
- Nome della barca: tasto `n` nelle impostazioni; viene salvato in `config.txt`.
- Da fare: allarme sonoro e ritrasmissione mesh.
