// Czech for the gauge part - see ../index.ts. English text -> Czech.
export const gauge: Record<string, string> = {
  // gauge.tsx
  'Not the gauge’s code': 'Toto není kód budíku',
  'Scan the QR on the gauge’s WiFi screen.': 'Načtěte QR kód na obrazovce Wi-Fi budíku.',
  'Gauge settings': 'Nastavení budíku',
  'Scan a different gauge': 'Načíst jiný budík',
  // One word for "Set up" app-wide: the log key's button uses it too.
  'Set up': 'Nastavit',
  'Tap through the views on the gauge to the WiFi screen, then scan its QR code. The app keeps the password, so this is needed once.':
    'Proklikejte se na budíku k obrazovce Wi-Fi a načtěte její QR kód. Aplikace si heslo zapamatuje, takže to stačí jednou.',
  'Scan the gauge’s QR': 'Načíst QR kód budíku',
  Connection: 'Připojení',
  Network: 'Síť',
  Disconnect: 'Odpojit',
  Connect: 'Připojit',
  'Joins the gauge’s WiFi - the gauge must be awake (car on, or a door opened). New drives come over by themselves, and its live readings and system show here. Meanwhile the phone has no internet, so maps will not load.':
    'Připojí se k Wi-Fi budíku - budík musí být vzhůru (auto zapnuté nebo otevřené dveře). Nové jízdy se přenesou samy a zobrazí se tu živé hodnoty a systém. Telefon mezitím nemá internet, takže se nenačtou mapy.',
  Live: 'Živě',
  'Paused while the logs sync.': 'Pozastaveno během synchronizace záznamů.',
  'Reading…': 'Načítám…',
  Logs: 'Záznamy',
  'Waiting for upload': 'Čeká na nahrání',
  'On the phone': 'V telefonu',
  'On the gauge': 'V budíku',
  '{n} files|{n} files': '{n} soubor|{n} soubory|{n} souborů',
  '{n} files, {size}|{n} files, {size}': '{n} soubor, {size}|{n} soubory, {size}|{n} souborů, {size}',
  'To download': 'Ke stažení',
  'up to date': 'aktuální',
  'Some files are smaller than the copies here; they will be fetched again whole.':
    'Některé soubory jsou menší než kopie v telefonu; stáhnou se znovu celé.',
  'New logs are pulled by themselves whenever the phone connects.':
    'Nové záznamy se stáhnou samy, kdykoli se telefon připojí.',
  Cancel: 'Zrušit',
  Pull: 'Stáhnout',
  'Check again': 'Zkontrolovat znovu',
  Actions: 'Akce',
  'Set the gauge’s clock from this phone': 'Nastavit hodiny budíku podle telefonu',
  Clock: 'Hodiny',
  'Zero the g-meter here': 'Vynulovat g-metr zde',
  'G-meter': 'G-metr',
  'Zero the g-meter?': 'Vynulovat g-metr?',
  'Only with the car level and standing still.': 'Jen když auto stojí na rovině a v klidu.',
  Zero: 'Vynulovat',
  'not connected': 'nepřipojeno',
  'bus error': 'chyba sběrnice',
  answering: 'odpovídá',
  'not answering': 'neodpovídá',
  Car: 'Auto',
  Oil: 'Olej',
  Coolant: 'Chladicí kapalina',
  // "Boost" in Czech too - what drivers call it ("plnicí tlak" read oddly).
  Boost: 'Boost',
  Voltage: 'Napětí',
  'no module': 'bez modulu',
  'fix, {n} satellites|fix, {n} satellites': 'poloha, {n} satelit|poloha, {n} satelity|poloha, {n} satelitů',
  'no fix yet ({n} satellites)|no fix yet ({n} satellites)':
    'zatím bez polohy ({n} satelit)|zatím bez polohy ({n} satelity)|zatím bez polohy ({n} satelitů)',
  'not set': 'nenastaveno',

  // gauge-settings.tsx
  Settings: 'Nastavení',
  Firmware: 'Firmware',
  '{name} is not a .bin': '{name} není soubor .bin',
  'Update firmware?': 'Aktualizovat firmware?',
  '{name}, {size} MB. Takes ~30 s; the gauge restarts.': '{name}, {size} MB. Trvá ~30 s; budík se restartuje.',
  Update: 'Aktualizovat',
  'Not connected': 'Nepřipojeno',
  'The settings are read from the gauge itself. Connect to change them or to update its firmware.':
    'Nastavení se čte přímo z budíku. Připojte se, chcete-li ho změnit nebo aktualizovat firmware.',
  'Set the gauge up first, on the Gauge screen.': 'Nejdřív budík nastavte, na obrazovce Budík.',
  'Reading the gauge’s settings…': 'Načítám nastavení budíku…',
  'Save 1 change|Save {n} changes': 'Uložit {n} změnu|Uložit {n} změny|Uložit {n} změn',
  'No changes': 'Žádné změny',
  'Pick a car_gauge_v1 .bin from the phone’s files. If the new build fails, the gauge rolls back by itself.':
    'Vyberte soubor car_gauge_v1 .bin v telefonu. Pokud nová verze selže, budík se sám vrátí k předchozí.',
  'Update firmware': 'Aktualizovat firmware',

  // settings-schema.ts
  Display: 'Displej',
  Power: 'Napájení',
  Warnings: 'Varování',
  Tyres: 'Pneumatiky',
  Logging: 'Záznam',
  Brightness: 'Jas',
  'Dim at night': 'V noci ztlumit',
  'Night brightness': 'Noční jas',
  'Wake on motion': 'Probudit pohybem',
  '0 = off. Below 30 is taken as 30.': '0 = vypnuto. Méně než 30 se bere jako 30.',
  'Sleep after': 'Uspat po',
  'Oil ready': 'Olej zahřátý',
  'Oil warning': 'Varování oleje',
  'Coolant warning': 'Varování chladiva',
  'Front low': 'Přední min.',
  'Front high': 'Přední max.',
  'Rear low': 'Zadní min.',
  'Rear high': 'Zadní max.',
  'What goes on the SD card': 'Co se ukládá na SD kartu',
  Off: 'Vypnuto',
  'Trips - GPS tracks only': 'Jízdy - jen trasy GPS',
  'Normal - tracks + drive data': 'Běžný - trasy + data jízdy',
  'Full - + raw diagnostic bytes': 'Úplný - + surové diagnostické bajty',
  'Applies at once and starts new files. Track sessions (.vbo) are recorded at every level but Off; the sleep log always is. Full is for checking new values against the car.':
    'Platí hned a začne nové soubory. Jízdy na okruhu (.vbo) se zaznamenávají na každé úrovni kromě Vypnuto; záznam uspání vždy. Úplný slouží k ověřování nových hodnot na autě.',
  'New trip after a stop of': 'Nová jízda po zastávce',
  'not a number': 'není číslo',
  'whole numbers only': 'jen celá čísla',

  // screens.ts, screen-order-card.tsx
  'Oil & coolant': 'Olej a chladivo',
  GPS: 'GPS',
  Track: 'Okruh',
  WiFi: 'Wi-Fi',
  Screens: 'Obrazovky',
  'The order a swipe goes through on the gauge. WiFi stays: it is the way to the phone.':
    'Pořadí, v jakém je budík projíždí přejetím. Wi-Fi zůstává: je to cesta k telefonu.',
  'Move up': 'Posunout nahoru',
  'Move down': 'Posunout dolů',

  // scan.tsx
  'The camera reads the QR code on the gauge’s WiFi screen. Nothing else is recorded.':
    'Fotoaparát načte QR kód na obrazovce Wi-Fi budíku. Nic jiného se nezaznamenává.',
  'Allow camera': 'Povolit fotoaparát',
  'That is not the gauge’s WiFi code.': 'Toto není kód Wi-Fi budíku.',
  'Point at the QR on the gauge’s WiFi screen.': 'Namiřte na QR kód na obrazovce Wi-Fi budíku.',

  // server.tsx
  'Works - the server holds 1 file.|Works - the server holds {n} files.':
    'Funguje - na serveru je {n} soubor.|Funguje - na serveru jsou {n} soubory.|Funguje - na serveru je {n} souborů.',
  Server: 'Server',
  Address: 'Adresa',
  Token: 'Token',
  "TOKEN from the server's .env": 'TOKEN ze souboru .env serveru',
  'The server is backend/ in the repo. The phone reaches it over mobile data, so the VPN must be up.':
    'Server je backend/ v repozitáři. Telefon se k němu dostane přes mobilní data, takže musí běžet VPN.',
  'Disconnect from the gauge to test: while connected, the app can only reach the gauge.':
    'Pro test se od budíku odpojte: dokud je připojený, aplikace se dostane jen k budíku.',
  Test: 'Otestovat',
  Save: 'Uložit',

  // system-card.tsx
  System: 'Systém',
  'RAM free': 'Volná RAM',
  '{free} of {total}, lowest {min}': '{free} z {total}, nejméně {min}',
  'PSRAM free': 'Volná PSRAM',
  '{used} of {total}': '{used} z {total}',
  'SD card': 'SD karta',
  'not available': 'není k dispozici',
  'Car requests ok / failed': 'Dotazy na auto ok / selhalo',
  'Chip temperature': 'Teplota čipu',
  'Started by': 'Spuštěno',
  Up: 'Běží',

  // pod-chip.tsx
  'Synced {when}': 'Synchronizováno {when}',
  'Set up the gauge': 'Nastavit budík',
  'Connecting…': 'Připojování…',
  Connected: 'Připojeno',
  'Syncing {at}/{of}': 'Synchronizace {at}/{of}',
  'Checking…': 'Kontrola…',
  'Sync failed': 'Synchronizace selhala',
  'Gauge: {status}': 'Budík: {status}',

  // upload-card.tsx
  'Cancelled after 1 file.|Cancelled after {n} files.':
    'Zrušeno po {n} souboru.|Zrušeno po {n} souborech.|Zrušeno po {n} souborech.',
  'Uploaded 1 file, {size}.|Uploaded {n} files, {size}.':
    'Nahrán {n} soubor, {size}.|Nahrány {n} soubory, {size}.|Nahráno {n} souborů, {size}.',
  Upload: 'Nahrát',
  Waiting: 'Čeká',
  '1 file, {size}|{n} files, {size}': '{n} soubor, {size}|{n} soubory, {size}|{n} souborů, {size}',
  nothing: 'nic',
  'Change server': 'Změnit server',
  'Set up the server': 'Nastavit server',
};
