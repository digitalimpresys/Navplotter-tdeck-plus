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
- **Guardia all'ancora**: tasto `f` ("dai fondo") o pulsante ⚓ della web app. Il T-Deck memorizza il punto e il raggio (20-150 m, o a piacere da 10 a 500) e controlla da solo la distanza, anche con il telefono spento. Sulla mappa: cerchio verde con l'ancora al centro e la linea fino alla barca, distanza/raggio in alto a sinistra. Se la barca resta fuori dal cerchio per 8 secondi (o il GPS manca da un minuto) parte l'allarme: fascia rossa e cinque bip ogni due secondi, anche con l'allarme sonoro spento nelle impostazioni. Rientrata per 20 secondi, l'allarme cessa da solo. `x` o un tocco sulla fascia lo silenzia per cinque minuti. Per salpare: `f` due volte entro tre secondi, o "Salpa" nella web app. Punto, raggio e guardia restano in `config.txt`: dopo un riavvio la guardia riprende.
- **Pagina bussola**: rosa con la prora in alto, punti cardinali, freccia verso la destinazione, rilevamento, distanza e gradi di accostata. La prora è la rotta GPS (la scheda non ha bussola magnetica): sotto 0,5 nodi resta l'ultimo valore valido.
- **SD**: stessi file della CYD (`config.txt`, `waypoints.txt`, `routes.txt`), quindi la stessa scheda funziona su entrambi.
- Le impostazioni mostrano se GPS, SD, touch e tastiera sono stati trovati.
- **Mappe a tile da SD**: `/tiles/z/x/y.jpg` (schema XYZ, Web Mercator, JPEG 256x256). Lo zoom va a livelli interi, limitati a quelli presenti sulla scheda; dove manca una tile resta lo sfondo azzurro.

## Bussola esterna (ESP-NOW)

Un ESP32 con un sensore BNO08x (programma in `bussola-imu/trasmettitore_imu`) trasmette via ESP-NOW, sul canale WiFi 1, prora magnetica, sbandamento, beccheggio e velocità di accostata in frasi NMEA. Il T-Deck li riceve senza abbinamenti: basta che il trasmettitore sia acceso.

**È facoltativa**: il plotter funziona in tutto anche senza, con la prora dalla rotta GPS. La bussola viene usata solo se viene rilevata, e compare nelle impostazioni (`Bussola:ok`, o `persa` se smette di trasmettere) solo dopo essere stata sentita. Con il WiFi del plotter acceso l'ascolto non costa nulla; con il WiFi spento la radio resta accesa per cercarla solo due minuti, e se non arriva nulla (o la bussola tace per due minuti) si spegne per risparmiare batteria fino al prossimo riavvio o alla prossima accensione/spegnimento del WiFi.

- **Pagina bussola**: con la bussola collegata la prora viene da lì (scritta "da bussola cal x/3"; verde da 2/3 in su) ed è valida anche da fermi; compaiono anche sbandamento e velocità di accostata. Senza dati per 3 secondi si torna da sola alla rotta GPS ("da COG").
- **Mappa**: il triangolo della barca punta dove punta la prua.
- **Declinazione magnetica**: la prora della bussola è magnetica; per avere la prora vera si imposta la declinazione del luogo (positiva verso est) con `decl 4.5` dal monitor seriale o `magDecl=` in `config.txt`.
- **Invariati**: i pacchetti LoRa e il calcolo del CPA usano sempre la rotta GPS.
- **Web app**: nella pagina bussola la prora viene dalla bussola ("da bussola · cal x/3"), sotto c'è un inclinometro con lo sbandamento e tra i dati compaiono sbandamento e velocità di accostata; sulla mappa il simbolo della barca segue la prua. I nodi senza bussola (o se i dati non arrivano) restano sulla rotta GPS come prima.
- Dal monitor seriale `imu` stampa gli ultimi dati ricevuti; la web app trova i dati in `/api/state`, alla voce `att`.
- **Limite**: ESP-NOW funziona solo sul canale in cui è la radio. La rete del plotter è sul canale 1, come la bussola; collegato alla WiFi di bordo il plotter segue invece il canale del router e la bussola non arriva (si torna alla rotta GPS). Con il WiFi spento la radio si accende solo per cercare la bussola (vedi sopra).

## Stima senza GPS e scatola nera

La stessa stima di Navplotter-Linux (`include/nav_stima.h` è la traduzione riga per riga di `navplotter/stima.py`, con gli stessi numeri): un filtro di Kalman segue il GPS "in ombra" e, quando il fix manca da 5 secondi, porta avanti la barca con l'ultima rotta e velocità. Con la bussola segue anche le virate (conta solo quanto gira la prua: declinazione e bussola montata storta non contano). Dopo 30 minuti, o quando l'incertezza supera le 2 miglia, la stima viene dichiarata non affidabile.

- **Mappa del T-Deck**: senza GPS la barca è un triangolo arancio vuoto con la scritta "STIMA", dentro il cerchio che la contiene 95 volte su 100; in alto "STIMA: senza GPS da m:ss, entro …". Un GPS che salta lontano dalla rotta seguita viene segnalato. La mappa che segue la barca segue la stima.
- **Pagina bussola e barra di stato**: rilevamento e distanza della destinazione continuano dalla posizione stimata (in arancio), con la riga "STIMA (no GPS)".
- **Web app**: il modulo `web/navplotter-stima.js`, lo stesso file di Navplotter-Linux: barca stimata con l'ellisse, avviso sopra la carta, scheda "Stima" in Impostazioni con lo stato della bussola e lo scaricamento delle registrazioni.
- **Mai trasmessa**: come sul Raspberry, la posizione stimata non va nei pacchetti LoRa, nel tracciato, nella guardia all'ancora né nel calcolo del CPA.
- **Scatola nera**: in `/log` sulla SD, un file al giorno (`2026-11-27.jsonl`, data UTC del GPS) con le letture grezze di GPS e bussola e la stima ogni 10 s, nello stesso formato del Raspberry, così si riproducono con `tools/stima_replay.py` di Navplotter-Linux. Tiene 30 giorni (`logDays=` in `config.txt`); i file non sono compressi. Dalla web app (o `/api/log.zip`) si scaricano gli ultimi 3 giorni: durante lo scaricamento il T-Deck non fa altro, meglio farlo in porto.
- Dal monitor seriale `stima` stampa lo stato della stima e della scatola nera.
- Lo stato (`/api/state`) ha gli stessi blocchi di Navplotter-Linux: `dr` (la stima), `att` (la bussola: prora magnetica, sbandamento, beccheggio, calibrazione; qui anche `rot` e `decl`) e `log`.

## Tastiera

| Tasto | Azione |
|---|---|
| m / w / r / s | mappa / waypoint / rotte / impostazioni |
| b | pagina bussola |
| g | nella lista waypoint: imposta (o annulla) la destinazione |
| i o + / o o - | zoom + / zoom - |
| c | centra sulla barca e la segue (la barca resta al centro); spostando la mappa con la trackball si smette di seguirla |
| a | waypoint sulla posizione della barca |
| f | guardia all'ancora: dai fondo qui; con la guardia attiva, due volte entro tre secondi per salpare |
| x | durante un allarme: lo silenzia (ancora: cinque minuti) |
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

## Traffico LoRa

I plotter con questo firmware si scambiano via radio nome, posizione, rotta e velocità, e si vedono a vicenda sulla mappa. Non è AIS: si vedono solo le barche con questo stesso firmware.

- Radio: 869,525 MHz, 125 kHz, SF9, 22 dBm (banda europea 869,4-869,65 MHz, 10% del tempo). La frequenza si cambia con `loraFreq=` in `config.txt`.
- Un pacchetto ogni 10 secondi in movimento e ogni 30 da fermi, solo con il fix GPS.
- Le altre barche compaiono sulla mappa come triangoli blu con nome e vettore di rotta; grigie dopo un minuto senza notizie, tolte dopo tre.
- Allarme di abbordo: se il massimo avvicinamento previsto (CPA) è sotto 0,1 miglia entro 5 minuti, la barca diventa rossa e compare una fascia rossa in alto.
- Privacy: la spunta "Trasmetti pos." nelle impostazioni (tasto `p`) autorizza a trasmettere la propria posizione. Tolta, la barca resta invisibile agli altri (sulla mappa compare "NASCOSTA") ma continua a riceverli, a dare gli allarmi e a ripetere i loro pacchetti.
- Tipi di nodo: oltre alle barche (triangolo) si vedono persone (anello con punto) e oggetti (rombo), come impostato sul nodo che trasmette; nell'elenco sono segnati con P e O.
- Tasto `t` (o "Barche vicine" nelle impostazioni): elenco con distanza, rilevamento, CPA e segnale.
- Nome della barca: tasto `n` nelle impostazioni; viene salvato in `config.txt`.
- Allarme sonoro: tre bip ogni tre secondi finché dura il pericolo. Si spegne con la spunta "Allarme sonoro" nelle impostazioni (tasto `x`); "Prova suono" fa sentire i bip. Con un allarme in corso, il tasto `x` o un tocco sulla fascia rossa lo tacita temporaneamente, senza cambiare l'impostazione: dura un minuto (o meno, se l'allarme rientra prima), e una nuova barca in allarme torna a suonare.
- Mesh: ogni apparato ritrasmette una volta i pacchetti delle altre barche (fino a due salti), dopo un'attesa casuale e solo se non li ha già sentiti ripetere da un altro. Nell'elenco una barca ricevuta così è segnata "ripetuta".
- Formato del pacchetto, parametri radio e logica mesh sono in `include/traffic_packet.h`, identico nei progetti dei simulatori.

## Messaggi

Brevi testi (al massimo 63 caratteri) tra NavPlotter, via LoRa e attraverso la mesh, accanto alle posizioni:

- a **una barca**: il messaggio viene ripetuto finche' quella barca non conferma che e' arrivato ("ricevuto"), e chi l'ha mandato viene avvisato di nuovo quando qualcuno la' lo ha letto ("letto"). Dopo quattro tentativi senza conferma compare "NESSUNA RISPOSTA";
- a **tutti** quelli in portata: inviato due volte, senza chiedere conferma.

Sul plotter: tasto **q** per la pagina messaggi (Spazio cambia destinatario, Enter scrive). Dalla pagina barche vicine (**t**), ordinata per distanza con le piu' vicine in alto, Spazio sceglie una barca ed Enter, o un tocco sulla riga, apre i messaggi con lei. Durante un allarme di abbordo il tasto **v** (o il pulsante sotto la fascia rossa) apre un messaggio gia' indirizzato alla barca in allarme. Dal telefono: pagina **Chat** della web app.

Un messaggio in arrivo compare sopra qualunque pagina, con un suono ripetuto finche' non viene chiuso. Ricevono solo gli apparati con questo programma, accesi e in portata, e un pacchetto puo' perdersi: e' una comodita', non un sistema di sicurezza. I messaggi stanno in memoria (gli ultimi 12) e si perdono allo spegnimento. Un Segnalatore che dorme non riceve nulla.

Il protocollo e' in `include/nav_messages.h`, che deve essere lo stesso file in tutti i NavPlotter che hanno i messaggi.

## Tracciato

Il plotter registra da solo il tracciato della barca: un punto ogni 25 metri percorsi, nessuno da fermi. I punti stanno in memoria (al massimo 4000, circa 54 miglia; quando e' pieno viene tolto il quarto piu' vecchio) e vengono salvati sulla SD ogni paio di minuti, nel file `track.bin`. Sulla mappa e' la linea arancione. Dal telefono, nella web app, pagina Waypoint, linguetta Tracciato: lunghezza, inizio e durata, salvataggio come file GPX, nuovo tracciato. Il codice e' in `include/nav_track.h`, uguale a quello del nodo Heltec.

## Simulatori di barca

Nella cartella `simulatori/` ci sono due progetti PlatformIO per **Heltec WiFi LoRa 32 V3 (868 MHz)** che fingono di essere altre barche, per provare traffico, allarmi e mesh senza altre T-Deck:

- `simulatori/sim-barca-pippa`: barca "Pippa", arriva da nord-est.
- `simulatori/sim-barca-serpico`: barca "Serpico", arriva da nord-ovest.

Ognuno ha il suo README. Si compilano e caricano dalla propria cartella con `pio run -t upload`.

## Altre funzioni

- **Batteria**: tensione e carica stimata nella pagina impostazioni.
- **Inseguimento**: dopo il tasto `c` la barca resta al centro della mappa finché la mappa non viene spostata a mano.

## Nodo Heltec con web app

`navplotter-heltec/` è il firmware per **Heltec WiFi LoRa 32 V3 con GPS esterno**: fa lo stesso lavoro del plotter (GPS, traffico LoRa, mesh, allarme di abbordo) ma senza schermo cartografico, e mostra tutto su telefono o tablet tramite una web app servita dalla scheda via WiFi. Interfaccia in inglese e italiano. Dettagli nel suo README.

## Carte nautiche per la SD

`tools/render_charts.py` disegna le tile delle carte a partire dai dati liberi di OpenStreetMap, con uno stile da carta nautica: canali, bassifondi e barene distinti, moli e dighe, fari e fanali con la loro caratteristica, boe e briccole. Produce `tiles/z/x/y.jpg`, da copiare nella radice della SD (vale per la T-Deck e per il nodo Heltec).

Servono (scaricamenti liberi):

- uno o più estratti `.osm.pbf` da download.geofabrik.de (per l'alto Adriatico: `italy/nord-est`, `slovenia`, `croatia`);
- lo shapefile `land-polygons-split-4326` da osmdata.openstreetmap.de (la linea di costa come poligoni di terra);
- le librerie Python `osmium`, `shapely`, `pyshp`, `pillow`.

```
python tools/render_charts.py extract --bbox 44.35,12.0,45.85,14.0 --pbf nord-est-latest.osm.pbf slovenia-latest.osm.pbf croatia-latest.osm.pbf --land land-polygons-split-4326/land_polygons.shp --cache adriatico.pkl
python tools/render_charts.py preview --cache adriatico.pkl --zoom 14 --center 45.43,12.34 --size 4x3 --png prova.png
python tools/render_charts.py render  --cache adriatico.pkl --zoom 8-16 --out nautical-tiles
```

Gli zoom bassi coprono tutta la zona; dal 14 in su vengono disegnate solo le tile vicine al mare, e quelle di solo mare aperto non vengono scritte (il plotter mostra lì lo stesso azzurro).

Con `--jobs 4` le tile vengono disegnate da 4 processi insieme (Linux e macOS).

### Carte di tutto l'Adriatico, pronte

L'automazione GitHub **Carte Adriatico** (`.github/workflows/carte.yml`) scarica i dati e disegna le carte per sette zone: alto Adriatico (Trieste–Venezia–Ravenna, Slovenia, Istria ovest), Marche e Abruzzo fino a Pescara, Quarnaro, Dalmazia nord, Dalmazia centrale, Dubrovnik e Montenegro, Albania fino al canale d'Otranto. Le zone sono disegnate in parallelo, poi `tools/pack_tiles.py` le unisce e le divide in zip sotto i 2 GB, pubblicati nella release `carte-adriatico` con un'anteprima per zona.

Per la SD: scaricare tutti i file `carte-adriatico-parteN.zip` e scompattarli nella radice della scheda. Si riavvia da Actions → Carte Adriatico → Run workflow (si può indicare solo alcune zone, per esempio `quarnaro dalmazia-nord`).

**Non sono carte per la navigazione**: OpenStreetMap non contiene le profondità e i segnalamenti sono inseriti da volontari. Dati © OpenStreetMap contributors, licenza ODbL.

## Web app dal telefono

Il plotter crea anche una sua rete WiFi e serve la stessa web app del nodo Heltec, con mappa, bussola, barche vicine, waypoint, rotte e impostazioni sullo schermo più grande del telefono.

1. Collegare il telefono alla rete `NavPlotter-<nome barca>` (password di fabbrica `navplotter`; rete, password e indirizzo sono mostrati nella pagina impostazioni).
2. Aprire `http://192.168.4.1`.

Le carte arrivano al telefono dalla SD del plotter, quindi non serve internet. Waypoint, rotte e destinazione sono in comune: ciò che si cambia sul plotter compare sul telefono e viceversa. Il WiFi si spegne con la spunta "WiFi" nelle impostazioni.

La web app è `web/index.html` (una copia è in `navplotter-heltec/web/`: vanno tenute uguali); `tools/embed_web.py` la comprime in `src/web_page.h` a ogni compilazione.
