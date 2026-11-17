// Czech for the trips part - see ../index.ts. English text -> Czech.
export const trips: Record<string, string> = {
  // app/index.tsx - the drives list
  'Reading new drives… {i} of {n}': 'Načítám nové jízdy… {i} z {n}',
  'Syncing {at} of {of}: {got} of {total}': 'Synchronizace {at} z {of}: {got} z {total}',
  'Could not read it: {error}': 'Nelze přečíst: {error}',
  'Saved': 'Uloženo',
  'Save': 'Uložit',
  'Saving…': 'Ukládám…',
  'Save all to a folder': 'Uložit vše do složky',
  'Settings': 'Nastavení',
  'No drives on the phone yet. Tap the gauge at the top to connect - new drives come over by themselves.':
    'V telefonu zatím nejsou žádné jízdy. Klepnutím na budík nahoře se připojte - nové jízdy se přenesou samy.',
  '1 drive|{n} drives': '{n} jízda|{n} jízdy|{n} jízd',
  'Show 1 drive|Show {n} drives': 'Zobrazit {n} jízdu|Zobrazit {n} jízdy|Zobrazit {n} jízd',
  'Reading…': 'Načítám…',
  'Hide the 1 very short drive|Hide the {n} very short drives':
    'Skrýt {n} velmi krátkou jízdu|Skrýt {n} velmi krátké jízdy|Skrýt {n} velmi krátkých jízd',
  'Show 1 very short drive (under 200 m)|Show {n} very short drives (under 200 m)':
    'Zobrazit {n} velmi krátkou jízdu (pod 200 m)|Zobrazit {n} velmi krátké jízdy (pod 200 m)|Zobrazit {n} velmi krátkých jízd (pod 200 m)',
  'Undated drive': 'Jízda bez data',
  // app/fuel.tsx and its charts (fuel-charts.tsx); the list's way to it
  'Fuel': 'Palivo',
  'More about fuel': 'Víc o palivu',
  'Reading the car data… {at} of {of}': 'Načítám data z auta… {at} z {of}',
  '{n} drives could not be read.': '{n} jízd se nepodařilo načíst.',
  'Overall': 'Celkem',
  'Corrected by the pump': 'Opraveno podle pumpy',
  'No drive with the car’s fuel figure yet.': 'Zatím žádná jízda s údajem auta o palivu.',
  'The car’s figure against the pump': 'Údaj auta proti pumpě',
  'The car reports the fuel it uses; added up over the drives between two fill-ups, it should be what went in at the second. Type in the pump’s litres at each fill-up - the tank level alone is coarse.':
    'Auto hlásí, kolik paliva spotřebuje; sečteno přes jízdy mezi dvěma tankováními by to mělo být, kolik se natankovalo podruhé. Zadejte u každého tankování litry z pumpy - samotná hladina v nádrži je hrubá.',
  'The car’s figure is {pct} {dir}, over {n} stretches by the {by}.':
    'Údaj auta je o {pct} {dir}, podle {by} z {n} úseků.',
  'low': 'nižší',
  'high': 'vyšší',
  'pump': 'pumpy',
  'tank level': 'hladiny v nádrži',
  'Two fill-ups made while parked are needed, with drives between them.':
    'Jsou potřeba dvě tankování při stání a jízdy mezi nimi.',
  'car {car} l  ·  burned {burned}': 'auto {car} l  ·  spáleno {burned}',
  'Fill-ups': 'Tankování',
  'litres': 'litry',
  'Consumption per drive': 'Spotřeba po jízdách',
  '{n} drives, {v} l/100 overall': '{n} jízd, celkem {v} l/100',
  'average': 'průměr',
  'a drive': 'jízda',
  'the last 8 drives, by distance': 'posledních 8 jízd, podle vzdálenosti',
  'Fuel per week': 'Palivo po týdnech',
  '{l} l in {n} weeks': '{l} l za {n} týdnů',
  'week of {d}': 'týden od {d}',
  // Merging drives (merge.ts): the list's choosing, a merged row
  '1 drive chosen|{n} drives chosen': '{n} vybraná jízda|{n} vybrané jízdy|{n} vybraných jízd',
  'Tap more drives to merge them': 'Klepněte na další jízdy ke spojení',
  'Merge': 'Spojit',
  'Merged from 1 drive|Merged from {n} drives': 'Spojeno z {n} jízdy|Spojeno ze {n} jízd|Spojeno z {n} jízd',
  // ...and its screen
  'Split into its drives': 'Rozdělit na jednotlivé jízdy',
  'Merged from {n} drives - its menu splits it again.': 'Spojeno z {n} jízd - v nabídce ji lze znovu rozdělit.',
  'With the stops': 'Včetně zastávek',
  'time': 'čas',
  'top': 'max. rychlost',
  // "Boost" in Czech too - what drivers call it ("plnicí tlak" read oddly).
  'boost': 'boost',
  'oil': 'olej',
  'fuel': 'palivo',
  'consumption': 'spotřeba',
  'No GPS track - car data only.': 'Bez trasy GPS - jen data z auta.',
  'Not all of this drive is here yet - the rest comes with the next upload or sync.':
    'Tahle jízda tu ještě není celá - zbytek přijde s dalším nahráním nebo synchronizací.',
  'This drive is cut off - the gauge lost power or restarted before it ended.':
    'Jízda je useknutá - budík ztratil napájení nebo se restartoval, než skončila.',
  // app/trip/[key].tsx's menu, for such a drive
  'Mark as finished': 'Označit jako dokončenou',
  'Mark as not finished': 'Označit jako nedokončenou',
  'Filled up': 'Natankováno',
  'which fuel?': 'jaké palivo?',
  'Consumption by fuel': 'Spotřeba podle paliva',

  // app/trip/[key].tsx - one drive
  'Files': 'Soubory',
  'That drive is no longer on the phone.': 'Tato jízda už v telefonu není.',
  'Rename this drive': 'Přejmenovat jízdu',
  'Name the start place': 'Pojmenovat místo startu',
  'Name the end place': 'Pojmenovat místo cíle',
  'Share the track (GPX)': 'Sdílet trasu (GPX)',
  'Share the car data (CSV)': 'Sdílet data z auta (CSV)',
  'Save to a folder': 'Uložit do složky',
  'Which logs?': 'Které logy?',
  'All': 'Všechny',
  'Last 7 days': 'Posledních 7 dní',
  'Delete this drive': 'Smazat jízdu',
  'Delete this drive?': 'Smazat tuto jízdu?',
  'It is removed from this phone and from the gauge’s card.': 'Smaže se z telefonu i z karty budíku.',
  'It is removed from this phone now, and from the gauge’s card the next time the phone connects.':
    'Z telefonu se smaže hned, z karty budíku při příštím připojení telefonu.',
  'Cancel': 'Zrušit',
  'Delete': 'Smazat',
  'Reading the logs…': 'Načítám záznamy…',
  'Preparing the file…': 'Připravuji soubor…',
  'Drive': 'Jízda',
  'Started': 'Začátek',
  'Distance': 'Vzdálenost',
  'Time': 'Čas',
  'Moving': 'V pohybu',
  'Top speed (GPS)': 'Max. rychlost (GPS)',
  'Average moving': 'Průměr v pohybu',
  'Car': 'Auto',
  'Peak boost': 'Max. boost',
  'Peak oil': 'Max. olej',
  'Oil at the end': 'Olej na konci',
  'Peak coolant': 'Max. chladicí kapalina',
  'Peak rpm': 'Max. otáčky',
  'Top speed (speedometer)': 'Max. rychlost (tachometr)',
  'Fuel used': 'Spotřebované palivo',
  'Consumption': 'Spotřeba',

  // app/name.tsx - naming a drive or a place
  'Rename the drive': 'Přejmenovat jízdu',
  'Rename your place': 'Přejmenovat vaše místo',
  'Name this place': 'Pojmenovat místo',
  'e.g. Track day': 'např. Okruh',
  'e.g. Home': 'např. Domov',
  'Only this drive. Without a name of its own it is called after where it started and ended.':
    'Jen tato jízda. Bez vlastního jména se jmenuje podle místa, kde začala a skončila.',
  'Every drive that starts or ends within 300 m of here is called by this name.':
    'Každá jízda, která začíná nebo končí do 300 m odsud, ponese toto jméno.',
  'Every spot of this place is renamed - every drive that starts or ends within 300 m of one.':
    'Přejmenují se všechna místa s tímto jménem - každá jízda, která u některého z nich do 300 m začíná nebo končí.',
  'Use the automatic name': 'Použít automatické jméno',
  'Forget this place': 'Zapomenout toto místo',

  // components/drive-chart.tsx
  'Speed': 'Rychlost',
  'Boost': 'Boost',
  // "Chladicí kapalina", as on the Gauge screen's Live card - one name for it.
  'Oil and coolant': 'Olej a chladicí kapalina',
  'Oil': 'Olej',
  'Coolant': 'Chladicí kapalina',
  'Voltage': 'Napětí',
  'Over the drive': 'Průběh jízdy',
  'car off': 'auto vypnuté',
  'engine off': 'motor vypnutý',
  'Drag across the charts to read them at any moment.': 'Přejeďte prstem po grafech a odečtěte hodnoty v kterémkoli okamžiku.',
  'At {at} of {total}': '{at} z {total}',

  // components/track-map.tsx
  'The map did not load ({reason}). It needs internet - not while connected to the gauge.':
    'Mapa se nenačetla ({reason}). Potřebuje internet - ne při připojení k budíku.',
  'Try again': 'Zkusit znovu',

  // components/places-card.tsx
  'Forget {name}?': 'Zapomenout místo {name}?',
  'Drives from and to it go back to the place names from the map.': 'Jízdy odsud a sem dostanou zpět názvy míst z mapy.',
  'Forget': 'Zapomenout',
  'Your places': 'Vaše místa',
  'None yet. On a drive, ⋮ → Name the start place (or the end) - e.g. Home - and every drive from or to there is called by it.':
    'Zatím žádná. U jízdy zvolte ⋮ → Pojmenovat místo startu (nebo cíle) - např. Domov - a každá jízda odtud nebo tam ponese toto jméno.',
  'Rename {name}': 'Přejmenovat {name}',
  'Forget {name}': 'Zapomenout {name}',
  // One place, several spots (places-card.tsx, name.tsx)
  '1 spot|{n} spots': '{n} bod|{n} body|{n} bodů',
  '{name} has 1 spot.|{name} has {n} spots.': '{name} má {n} bod.|{name} má {n} body.|{name} má {n} bodů.',
  'Rename all of them, or just this one?': 'Přejmenovat všechny, nebo jen tento?',
  'Forget all of them, or just this one?': 'Zapomenout všechny, nebo jen tento?',
  'Just this one': 'Jen tento',
  'All 1 spot|All {n} spots': 'Všechen {n} bod|Všechny {n} body|Všech {n} bodů',
  'Tap a name to rename it. Drives within 300 m of a place take its name.':
    'Klepnutím na jméno ho přejmenujete. Jízdy do 300 m od místa ponesou jeho jméno.',

  // components/lock-icon.tsx
  'Encrypted, not readable here': 'Šifrováno, zde nečitelné',
  'Encrypted': 'Šifrováno',

  // lib/places.ts - a loop's name
  'Around {place}': '{place} a okolí',

  // lib/weeks.ts - the drives list's week headings
  'Undated': 'Bez data',
  'This week': 'Tento týden',
  // A drive's date and time (lib/format.ts when())
  Today: 'Dnes',
  Yesterday: 'Včera',
  'Last week': 'Minulý týden',
};
