# Rally 2 Recomp

[English](README.md) | **Italiano**

Port nativo per Android del rally arcade Model 3 del 1998 (set MAME `srally2`).

Nessun emulatore: il codice PowerPC originale del gioco viene tradotto **sul tuo dispositivo** in codice ARM nativo (arm64 o ARM a 32 bit), e l'hardware arcade (grafica, audio, comandi) è reimplementato in modo nativo. Il risultato gira a piena velocità su normali telefoni e box Android TV.

**Non è incluso alcun codice o dato del gioco.** Serve il tuo `srally2.zip`.

## Download

Scarica l'ultima APK da [Releases](../../releases). Stato attuale: **beta**.

## Requisiti

- Android 8.0 o superiore
- OpenGL ES 3.2
- Consigliato: telefono di fascia media o superiore degli ultimi 3-4 anni. Su dispositivi più vecchi o economici funziona a risoluzione più bassa (l'impostazione predefinita *Auto* si adatta da sola).
- Il tuo `srally2.zip` (set ROM MAME)

## Per iniziare

1. Installa l'APK (se richiesto, consenti l'installazione da origini sconosciute).
2. Apri **Rally Recomp** e scegli il tuo `srally2.zip`.
   - Su Android TV l'app ha un suo file manager (chiavette USB e memoria interna). Concedi l'accesso ai file quando richiesto.
3. Al primo avvio l'app controlla lo zip e prepara il gioco. Ci vuole qualche secondo e succede una volta sola.
4. Inserisci il gettone, premi start, corri.

## Comandi

**Touch**: volante, acceleratore, freno, marce e cambio visuale a schermo. Si nascondono quando usi un gamepad e ricompaiono quando tocchi lo schermo.

**Gamepad**: qualsiasi controller USB o Bluetooth, collegabile in qualunque momento.

| Comando | Pad |
|---|---|
| Sterzo | stick sinistro (funziona anche la croce sinistra/destra) |
| Acceleratore / Freno | grilletto destro / sinistro (funziona anche la croce su/giù) |
| Visuale | A |
| Freno a mano | X o B |
| Marcia su / giù | RB / LB |
| Start / Gettone | Start / Back |

Per cambiare i tasti, metti un file `pad.ini` nella cartella dati dell'app (`Android/data/org.srally2/files/`):

```
steer=leftx
gas=righttrigger
brake=lefttrigger
view=a
handbrake=x,b
shiftup=rightshoulder
shiftdown=leftshoulder
start=start
coin=back
deadzone=0.10
```

**Telefono come controller**: ottimo con Android TV. Apri il menu impostazioni, scegli **Phone as controller (QR)** e inquadra il codice con il telefono. I due dispositivi devono essere sulla stessa rete Wi-Fi. Inclina il telefono per sterzare e usa i tasti a schermo per acceleratore, freno e marce. Il telefono vibra con il force feedback.

## Force feedback

Il force feedback originale del volante del cabinato (urti, salti, fondo stradale, carico) viene trasformato in vibrazione sul controller o, se non ce l'hai, sul telefono stesso. Puoi disattivarlo dal menu impostazioni.

## Impostazioni

- **Risoluzione**: *Auto* (si adatta per mantenere la piena velocità), *Native* (risoluzione dello schermo) o fissa da 480p a 2160p
- Widescreen
- Vibrazione on/off

## Problemi

Apri una [issue](../../issues) e allega il file di log:

```
Android/data/org.srally2/files/srally2.log
```

Indica anche il modello del dispositivo e la versione di Android.

## Compilare dai sorgenti

Vedi [android/README.md](android/README.md) (in inglese). Il repository contiene anche il ricompilatore statico (`lifter/`), il traduttore on-device (`xlat/`) e gli strumenti usati per verificarlo (`dev/`).

## Note legali

Questo progetto non contiene ROM, dump di ROM né codice derivato da esse. L'APK traduce il codice dallo zip che fornisci tu, sul tuo dispositivo, durante l'esecuzione. Usa solo un set ROM che hai il diritto di possedere.

Non affiliato né approvato dai titolari dei diritti del gioco originale. Tutti i marchi appartengono ai rispettivi proprietari.

Licenza GPLv3 (vedi [LICENSE](LICENSE) e [NOTICE](NOTICE)).

Sviluppato con l'assistenza di Claude (Anthropic).
