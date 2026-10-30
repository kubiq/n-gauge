// Czech for the gauge part - see ../index.ts. English text -> Czech.
export const gauge: Record<string, string> = {
  // gauge-settings.tsx: setting the gauge up, connecting, its actions
  'Not the gauge’s code': 'Toto není kód budíku',
  'Scan the QR on the gauge’s WiFi screen.': 'Načtěte QR kód na obrazovce Wi-Fi budíku.',
  'Gauge settings': 'Nastavení budíku',
  'Scan a different gauge': 'Načíst jiný budík',
  // One word for "Set up" app-wide: the log key's button uses it too.
  'Set up': 'Nastavit',
  'Tap through the views on the gauge to the WiFi screen, then scan its QR code. The app keeps the password, so this is needed once.':
    'Proklikejte se na budíku k obrazovce Wi-Fi a načtěte její QR kód. Aplikace si heslo zapamatuje, takže to stačí jednou.',
  'Scan the gauge’s QR': 'Načíst QR kód budíku',
  Network: 'Síť',
  Disconnect: 'Odpojit',
  Connect: 'Připojit',
  Logs: 'Záznamy',
  Cancel: 'Zrušit',
  Actions: 'Akce',
  'Set the gauge’s clock from this phone': 'Nastavit hodiny budíku podle telefonu',
  Clock: 'Hodiny',
  'Zero the g-meter here': 'Vynulovat g-metr zde',
  'G-meter': 'G-metr',
  'Zero the g-meter?': 'Vynulovat g-metr?',
  'Only with the car level and standing still.': 'Jen když auto stojí na rovině a v klidu.',
  Zero: 'Vynulovat',
  Car: 'Auto',
  Oil: 'Olej',
  Coolant: 'Chladicí kapalina',
  // "Boost" in Czech too - what drivers call it ("plnicí tlak" read oddly).
  Boost: 'Boost',
  Voltage: 'Napětí',

  // gauge-settings.tsx: the settings and firmware
  Settings: 'Nastavení',
  Firmware: 'Firmware',
  '{name} is not a .bin': '{name} není soubor .bin',
  'Update firmware?': 'Aktualizovat firmware?',
  '{name}, {size} MB. Takes ~30 s; the gauge restarts.': '{name}, {size} MB. Trvá ~30 s; budík se restartuje.',
  Update: 'Aktualizovat',
  'Not connected': 'Nepřipojeno',
  'The settings are read from the gauge itself - it must be awake (car on, or a door opened). Meanwhile the phone has no internet.':
    'Nastavení se čte přímo z budíku - ten musí být vzhůru (auto nastartované, nebo otevřené dveře). Telefon mezitím nemá internet.',
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
  'Applies at once and starts new files. Track sessions (.vbo) are recorded at every level but Off; the sleep log always is.':
    'Platí hned a začne nové soubory. Jízdy na okruhu (.vbo) se zaznamenávají na každé úrovni kromě Vypnuto; záznam uspání vždy.',
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
  'Syncing {at}/{of} · {pct} %': 'Synchronizace {at}/{of} · {pct} %',
  'Checking…': 'Kontrola…',
  'Sync failed': 'Synchronizace selhala',
  'Sync failed: {error}': 'Synchronizace selhala: {error}',
  'Gauge: {status}': 'Budík: {status}',
};
