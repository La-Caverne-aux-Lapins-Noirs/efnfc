# Pont navigateur pour efrits-nfc

Ce composant permet à `https://intra.efrits.fr` d'utiliser le lecteur NFC local
sans dépendre de WebNFC, WebUSB ou WebHID. Il fonctionne avec Firefox et les
navigateurs Chromium parce que le navigateur ne parle pas directement au lecteur :
il appelle un petit service HTTP lié uniquement à `127.0.0.1:38421`, lequel lance
le binaire `efrits-nfc` existant.

Le service refuse les requêtes dont l'en-tête `Origin` n'est pas exactement
`https://intra.efrits.fr`, refuse les autres `Host`, écoute exclusivement sur la
boucle locale, limite la taille des requêtes et n'expose que trois actions :
`/v1/ping`, `/v1/read`, `/v1/write`.

## Installation

Le CLI `efrits-nfc` doit déjà être installé et fonctionnel.

```sh
make -C browser
make -C browser install
sh ./browser/install-autostart.sh
```

Sous MSYS2, le préfixe par défaut est `$MINGW_PREFIX` (ou `/ucrt64`) : le pont
est donc installé à côté de `efrits-nfc.exe`. La commande suivante suffit :

```sh
make -C browser install
sh ./browser/install-autostart.sh
```

Sous Linux, l'installation par défaut est `~/.local/bin` et le script crée un
service systemd utilisateur (avec fallback vers l'autostart desktop).

Le pont recherche d'abord `efrits-nfc` à côté de lui, puis dans le `PATH`. La
variable d'environnement `EFRITS_NFC_BIN` peut forcer un chemin précis.

## API locale

- `GET http://127.0.0.1:38421/v1/ping`
- `POST http://127.0.0.1:38421/v1/read`
- `POST http://127.0.0.1:38421/v1/write` avec `{ "payload": "<base64>" }`

L'écriture crée un fichier `.nfc` temporaire de 32 octets, invoque le CLI normal,
puis supprime immédiatement ce fichier. La vérification écriture/relecture reste
donc assurée par `efrits-nfc` lui-même.

## Navigateurs

L'interface Infosphère utilise un `fetch()` CORS vers `http://127.0.0.1:38421`.
Firefox et Chromium peuvent charger une ressource loopback depuis une page HTTPS.
Les versions récentes de Chromium peuvent demander une autorisation d'accès au
réseau local / loopback au premier appel : elle doit être accordée à Infosphère.
Aucune extension de navigateur n'est nécessaire.
