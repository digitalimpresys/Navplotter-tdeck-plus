# Simulatore di barca "Pippa" per il plotter T-Deck

Firmware per **Heltec WiFi LoRa 32 V3 (868 MHz)** che finge di essere un'altra barca con il nostro nav plotter, per provare ricezione, visualizzazione e allarme di abbordo senza una seconda T-Deck.

## Come funziona

1. All'accensione cerca un modulo GPS sui pin 47 (TX) e 48 (RX) e ne rileva da sola la velocità. Il punto di riferimento è il proprio fix GPS; se il GPS manca o non ha il fix, usa la posizione ricevuta dal plotter.
2. Si mette a una certa distanza dal riferimento e comincia a "navigare", trasmettendo ogni 10 secondi nome, posizione, rotta e velocità.
3. Sul plotter compare come una barca qualsiasi.

Il tasto **PRG** cambia scenario:

| Scenario | Cosa fa | Cosa aspettarsi sul plotter |
|---|---|---|
| COLLISIONE | parte a 0,5 miglia a nord-est e punta dritto sul plotter a 6 nodi | allarme di abbordo (fascia rossa) |
| PASSA A 0.3 | stessa partenza, ma passa a 0,3 miglia | barca blu, nessun allarme |
| FERMA A 0.2 | ferma a 0,2 miglia a est | barca blu senza vettore, nessun allarme |
| GPS REALE | nessuna simulazione: trasmette posizione, rotta e velocità vere del GPS | la Heltec dove si trova davvero |

Passato il plotter di mezzo miglio, lo scenario ricomincia da capo.

Il display mostra scenario, distanza e rilevamento dal plotter, rotta e velocità simulate, nome e segnale del plotter, pacchetti inviati e ricevuti.

## Personalizzare

In cima a `src/main.cpp`: `SIM_BOAT_NAME` (nome, massimo 11 caratteri) e `SIM_TX_POWER_DBM` (2 dBm per le prove sul tavolo).

Formato del pacchetto, parametri radio e logica mesh sono in `include/traffic_packet.h`, che deve restare identico a quello del plotter (`include/traffic_packet.h` nella cartella principale) e dell'altro simulatore.

Tenendo premuto PRG per due secondi si attiva la prova mesh ("SOLO RIPET."): il simulatore accetta solo pacchetti arrivati tramite un ripetitore, come se le altre barche fossero fuori portata diretta.

## Compilare e caricare

```
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```
