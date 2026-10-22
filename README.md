# ESP32 Home GT

## Struttura

- `src/`: firmware, gestione relè, rete e API HTTP.
- `include/`: header e configurazione locale, come `secrets.h`.
- `lib/DeviceNetwork/`: connessione multi-Wi-Fi e registrazione rete del dispositivo.
- `lib/RelayRealtime/`: autenticazione del device e subscription Supabase Realtime.
- `data/`: pagine web servite dall'ESP32 tramite LittleFS.
- `docs/`: dashboard remota per GitHub Pages.
- `supabase/`: migration, RLS e Edge Function.

## Upload

Carica il firmware con il target PlatformIO `Upload`. Quando modifichi i file in `data/`, carica anche il filesystem con:

```sh
pio run --target uploadfs
```

Firmware e pagine web sono due immagini distinte: aggiornare una non aggiorna automaticamente l'altra.
La nuova tabella partizioni va installata una prima volta via USB con `pio run -e esp32dev_usb --target upload` e poi `pio run -e esp32dev_usb --target uploadfs`; per le successive modifiche firmware si può tornare all'ambiente OTA `esp32dev`. Se la porta seriale cambia, aggiorna `upload_port` nell'ambiente `esp32dev_usb`.

## Reti Wi-Fi e registro Supabase

Aggiungi le reti conosciute all'array `WIFI_CREDENTIALS` in `include/secrets.h` (partendo dal modello `include/secrets.example.h`). Il modulo seleziona una rete disponibile e riprova periodicamente se la connessione cade.

La registrazione di rete passa dall'Edge Function `register-device`, non direttamente dalla scheda alla tabella. Imposta un token casuale e univoco per dispositivo in `include/secrets.h`, quindi configura lo stesso token lato Supabase nella variabile `DEVICE_REGISTRY_TOKENS`, associata al suo `DEVICE_ID`. Per la subscription Realtime ogni ESP32 usa inoltre un utente Supabase Auth dedicato; non riutilizzare l'account umano. Non inserire mai la `service_role` key nel firmware o nel sito.

La migration crea lo stato desiderato/effettivo dei relè e i relativi permessi RLS; `set_relay_target` cambia il target e il trigger invia un Broadcast privato. Il firmware ascolta il canale senza polling e legge lo stato corrente solo quando si collega o riconnette. Per utenti Auth, associazione e deploy consulta [supabase/README.md](supabase/README.md).

SSID, IP locale e timestamp sono registrati a ogni connessione Wi-Fi; la dashboard remota riceve anche gli aggiornamenti di rete e dei relè via Realtime. Il sito locale in `data/` continua a funzionare senza Internet.