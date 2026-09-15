make clean
make
make install
make -C browser clean
make -C browser
make -C browser install
powershell.exe -NoProfile -Command \
  "Get-Process efrits-nfc-bridge -ErrorAction SilentlyContinue | Stop-Process -Force"
sh browser/install-autostart.sh
curl -H 'Origin: https://intra.efrits.fr' \
     http://127.0.0.1:38421/v1/ping
