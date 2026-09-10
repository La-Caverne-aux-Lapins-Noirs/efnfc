# efrits-nfc

Petit utilitaire EFRITS pour lire et programmer les cartes NFC avec un ACR1552U
via PC/SC. Le meme source fonctionne sous Windows/MSYS2 et sous Linux avec
pcsc-lite.

## Installation des dependances

Le script detecte automatiquement MSYS2 ou une distribution Linux Debian-like :

```sh
./install-dependencies.sh
```

Sous Debian/Ubuntu, il installe notamment `libpcsclite-dev`, `pcscd` et
`pcsc-tools`. Sous MSYS2 il installe la toolchain adaptee au shell courant
(UCRT64 recommande).

## Compilation

```sh
make
```

Le binaire produit est :

- Windows/MSYS2 : `efrits-nfc.exe`
- Linux : `efrits-nfc`

Le Makefile expose : `all`, `clean`, `fclean`, `re` et `install`.

### Installation

Sous Windows/MSYS2 :

```sh
make install
```

Le prefixe par defaut est le prefixe MinGW courant (`$MINGW_PREFIX`, ou
`/ucrt64`).

Sous Linux :

```sh
sudo make install
```

Le prefixe par defaut est `/usr/local`.

Dans les deux cas, il peut etre change :

```sh
make install PREFIX=/chemin/voulu
```

## Verification du lecteur sous Linux

Le lecteur doit etre visible par pcsc-lite. Vous pouvez verifier avant d'utiliser
l'outil :

```sh
pcsc_scan
```

Si PC/SC n'est pas disponible :

```sh
sudo systemctl start pcscd.socket
```

Sur la plupart des Debian recentes, pcscd fonctionne par activation socket.

## Utilisation

Lecture :

```sh
efrits-nfc
```

Le programme attend une carte, affiche son UID et indique si les pages 4..11
contiennent un payload EFRITS NFC v1 valide.

Ecriture :

```sh
efrits-nfc alice.dupont.nfc
```

Un unique argument est accepte. Si son extension n'est pas `.nfc`, le programme
quitte immediatement sans acceder au lecteur. Pour un `.nfc`, il verifie d'abord
le fichier, le type apparent de carte, ecrit les huit pages 4..11 une par une,
puis relit les 32 octets et exige une correspondance parfaite.

Le comportement et le format de carte sont identiques sous Windows et Linux.
Seule l'implementation PC/SC change : WinSCard sous Windows, pcsc-lite sous Linux.

## Format EFRITS NFC v1

Le fichier fait exactement 32 octets et est ecrit tel quel dans les pages 4..11 :

- 0..3 : `EFR1`
- 4 : version (`1`)
- 5 : taille du jeton (`16`)
- 6..7 : reserves (`0`)
- 8..23 : jeton aleatoire de 128 bits
- 24..27 : CRC32 IEEE des octets 0..23, big-endian
- 28..31 : `NFC!`

Il ne contient ni nom, ni codename, ni numero d'utilisateur, ni mot de passe/PIN.
