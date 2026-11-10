# Changelog

**English** · [Русский](#русский)

## Unreleased

**1.1.0 was withdrawn:** right after Install one head unit went into a reset loop that never
reached the menu, with the mod already off after its trial boot; the SD kill switch did not stop
it and the unit had to be reflashed. The cause was not found (no logs survived; the same package
boots normally on mQbEMU and in another car). This release makes sure no such loop can last.

### Safety
- **Boot-loop breaker:** while the mod's libraries are in the start-up list, every boot is
  counted; four boots in a row that do not stay up two minutes take the libraries out of
  `cpu.conf` by themselves, whether the mod is on or off. It runs first, 0.15 s after
  DisplayManager starts.
- **The SD kill switch unloads:** a card with `mst2-cluster-nav-disable` now takes the libraries
  out of `cpu.conf` (also when it is put in during the first two minutes after power-on), not
  only switches the mod off; Install puts them back.
- **Firmware check:** Install compares the 12 firmware files the hooks touch with
  MST2_EU_SK_ZR_P0480T and refuses any other build, and a `cpu.conf` another tool has edited,
  before anything is changed.
- **Dual navigation's Disable keeps the mod's guard:** the toolbox patch copies `runHMI.sh` to
  `runHMI.sh.bak` the first time it is enabled and copies it back on Disable. A copy older than
  Install brought back a `runHMI.sh` without the mod's block: the boot-loop breaker, the kill
  switch and the guard were gone while `cpu.conf` still loaded the libraries. Install now puts the
  block into that copy too, and Uninstall takes it out again.
- **Libraries do nothing where they do not belong:** in child processes, which inherit the
  start-up list's preload, every hook is a silent pass-through; nothing runs at load time; SAL's
  worker starts only with the mod on; DisplayManager does not look at the cluster frames while
  the mod is off.
- **Save diagnostics** also records every thread's state (`pidin`, shows a hung service), the
  preload lines of `cpu.conf`, the boot-loop counters, the firmware check and the HMI start
  script's class path with its backups (shows the toolbox dual navigation patch).

### Diagnostics
- **Android Auto log:** SAL's own connectivity log (Android Auto's receiver, AOA, the phone
  code), which the unit prints only as errors to its console, is copied into the mod's log from
  info level up, with each step of the mod's Android Auto session (cluster screen offered,
  channel open, setup, focus, first frame).
- **The log survives a reset:** while the mod runs, its log is also kept on the unit
  (`/tsd/var/mst2-cluster-nav/log`, the boot before as `log.old`), and Save diagnostics
  includes both: after a freeze, reboot and save.

### Fixed
- **Boot-loop breaker, after review:** each boot's count is synced to disk at once (a kernel
  fault or a hang skips the stock shutdown and would lose it); Install clears the count; a
  kill-switch card left in no longer remounts and rewrites a `cpu.conf` the mod is already out
  of; the firmware check also covers `libtsd.connectivity.common.so`, which the Android Auto log
  hooks depend on.
- **Install could reset the unit** when the card's `custom/mst2-cluster-nav` had no
  `preload.list`: the unit's awk (GNU Awk 3.1.5) crashes on a file it cannot open, and the unit
  resets on any crashed process. Every awk on the unit now reads standard input. Found on mQbEMU
  from the crash dump the unit wrote.
- **The `cpu.conf` line-end check never ran on the unit:** it made its carriage return with
  `printf`, which the unit does not have. It now uses the shell's own `print`.
- **Android Auto restarted the whole unit** the moment a phone connected while the mod ran
  (1.1.0): the cluster video endpoint the mod builds for Android Auto's receiver library sat on
  an odd address, the mutex inside it made the QNX kernel take an alignment fault, and the
  kernel shut the unit down. Its objects are now aligned. Found on mQbEMU with a full Android
  Auto session (laboratory phone); the session now runs with navigation, now playing and the
  cluster screen offered.
- **Install from the source code** ("Code → Download ZIP" instead of the release's
  `mst2-cluster-nav-<version>.zip`) stopped with only "MANIFEST is missing". It now lists what the
  folder holds and says to download the package under the release's Assets; a card with a
  complete package wins over one holding the source tree or an older copy.

## 1.1.0

### New
- **Android Auto on the cluster:** native arrows and lane symbols, now playing, and the phone's
  map in the Navigation view (re-encoded on the unit's own video encoder), through endpoints the
  unit's Android Auto receiver has but never offers.
- **Now playing from the phone** in the cluster's media view, CarPlay and Android Auto, with
  cover art; hidden while paused.
- **Android Auto roundabouts** drawn on the correct traffic side, with the exit taken from the
  angle travelled (or the exit number).
- **CarPlay map zoom-out** (Green Engineering Menu, off by default): 1.25× or 1.5× more map around
  the car. The phone draws a bigger cluster map; the unit decodes it, scales it down with a
  smoothing (bilinear) filter and encodes it for the cluster at 5 Mbit/s, the rate of the unit's
  own cluster video.
- **Event history that survives a reset:** every strike and switch-off of the self-repair guard,
  and every Install, Enable, Disable and trial, go to `/tsd/var/mst2-cluster-nav/history`.
  Status shows the last five, Save diagnostics all of them.
- The mod log records the CarPlay call path (connection and call states only — never names or
  numbers).

### Fixed
- **Zoom-out showed broken letters and a green band at the bottom.** The unit's 2D graphics core
  only takes the nearest pixel when it scales through the system compositor (a 1×1 filter, so
  whole rows and columns of thin strokes vanished); the scale now runs on the CPU with a proper
  filter. The re-encoded stream had no rate limit, so large key frames could be cut on their way
  to the cluster, which loses the bottom of the picture; it is now limited like the unit's own.
- **Zoom-out never reached the cluster** in the first 1.1 builds: the compositor refused every
  scaled frame.
- **The unit could reset right after boot** (3 of 7 boots on mQbEMU with the first 1.1 builds):
  the mod's background thread started before the head unit's memory allocator was ready. It now
  starts with the first phone.
- **"Turned itself off" too eagerly.** The self-repair guard switched the mod off after two HMI
  starts in a row that came within 3 minutes of one that had not settled — two quick
  off-and-on cycles were enough. It now takes three in a row (the native parts' crash guards
  likewise), which a real reset loop still reaches within minutes.
- **Status kept showing an old reason** after a trial boot ("did not settle twice"), so a trial
  that simply ended looked like the mod had switched itself off again. Arming a trial now clears
  the old reason; the history keeps it.
- The built-in navigation always wins: nothing is written while it guides, and the mod's "off"
  no longer wipes the built-in route.
- Unknown time and distance to the destination stay blank instead of showing 0:00 / 0.0 km.
- Cluster video: starts from the newest key frame, safe hand-over between phone sessions, the
  kill switch stops it at once.
- Install checks every file on the card (size and checksum) and every library's format before it
  changes anything, refreshes its backups after a firmware update and verifies each rewritten
  file line by line.
- Guidance ends after 120 s without a status from the phone; the street name changes only once
  the next maneuver is known.

### Removed
- Demo guidance and the duplicate "Phone map" menu entry.

### Not yet confirmed in a car
The new zoom-out scale and the Android Auto parts were developed and tested on mQbEMU and against
the unit's firmware; reports from cars are welcome (GEM → Save diagnostics).

---

## Русский

## Не выпущено

**1.1.0 отозван:** сразу после Install один блок ушёл в цикл перезагрузок, не доходя до меню,
хотя мод после пробного старта уже был выключен; файл-выключатель на SD не помог, блок пришлось
перепрошивать. Причина не найдена (логи не сохранились; тот же пакет нормально грузится на mQbEMU
и в другой машине). Этот выпуск делает так, чтобы такой цикл не мог продолжаться.

### Безопасность
- **Защита от цикла перезагрузок:** пока библиотеки мода в списке запуска, считается каждая
  загрузка; четыре загрузки подряд, не проработавшие двух минут, сами убирают библиотеки из
  `cpu.conf` — включён мод или нет. Работает первой, через 0,15 с после старта DisplayManager.
- **Выключатель на SD выгружает мод:** карта с `mst2-cluster-nav-disable` теперь убирает
  библиотеки из `cpu.conf` (и если её вставить в первые две минуты после включения), а не
  только выключает мод; Install возвращает их.
- **Проверка прошивки:** Install сверяет 12 файлов прошивки, которые трогают хуки, с
  MST2_EU_SK_ZR_P0480T и отказывается от любой другой сборки и от `cpu.conf`, изменённого другим
  инструментом, ничего не тронув.
- **Выключение двойной навигации больше не снимает сторожа мода:** патч тулбокса при первом
  включении копирует `runHMI.sh` в `runHMI.sh.bak`, а при выключении копирует обратно. Копия,
  сделанная до Install, возвращала `runHMI.sh` без блока мода: защиты от цикла перезагрузок,
  выключателя на SD и сторожа не было, а `cpu.conf` по-прежнему загружал библиотеки. Теперь Install
  добавляет блок и в эту копию, а Uninstall убирает его оттуда.
- **Библиотеки ничего не делают там, где им не место:** в дочерних процессах (они наследуют
  preload из списка запуска) каждый хук молча пропускает вызов; при загрузке ничего не
  запускается; рабочий поток SAL стартует только при включённом моде; DisplayManager не смотрит
  кадры приборки, пока мод выключен.
- **Save diagnostics** записывает ещё состояние каждого потока (`pidin`, видно зависшую службу),
  строки preload из `cpu.conf`, счётчики циклической перезагрузки, проверку прошивки и путь
  классов скрипта запуска HMI с его копиями (видно патч dual navigation тулбокса).

### Диагностика
- **Журнал Android Auto:** собственный журнал связи SAL (приёмник Android Auto, AOA, код
  телефона), который блок выводит только ошибками в консоль, копируется в журнал мода с уровня
  info, вместе с каждым шагом сеанса Android Auto мода (экран приборки предложен, канал открыт,
  setup, фокус, первый кадр).
- **Журнал переживает перезагрузку:** пока мод работает, журнал хранится и на блоке
  (`/tsd/var/mst2-cluster-nav/log`, предыдущая загрузка — `log.old`), Save diagnostics кладёт
  оба: после зависания перезагрузить и сохранить.

### Исправлено
- **Защита от цикла перезагрузок, после ревью:** счётчик каждой загрузки сразу пишется на
  носитель (сбой ядра или зависание проходят мимо штатного выключения и потеряли бы его); Install
  обнуляет счётчик; карта-выключатель, оставленная в слоте, больше не перемонтирует раздел и не
  переписывает `cpu.conf`, из которого мод уже убран; проверка прошивки охватывает и
  `libtsd.connectivity.common.so`, от которой зависят хуки журнала Android Auto.
- **Install мог перезагрузить блок**, если в `custom/mst2-cluster-nav` на карте нет
  `preload.list`: awk блока (GNU Awk 3.1.5) падает на файле, который не может открыть, а блок
  перезагружается при падении любого процесса. Теперь каждый awk на блоке читает стандартный ввод.
  Найдено на mQbEMU по дампу, который записал блок.
- **Проверка концов строк `cpu.conf` на блоке не работала:** символ CR она делала через `printf`,
  которого на блоке нет. Теперь — встроенным в оболочку `print`.
- **Android Auto перезапускал весь блок** в момент подключения телефона при работающем моде
  (1.1.0): объект видеоприёмника приборки, который мод собирает для библиотеки Android Auto,
  лежал по нечётному адресу, мьютекс в нём вызывал ошибку выравнивания в ядре QNX, и ядро
  останавливало блок. Объекты теперь выровнены. Найдено на mQbEMU в полном сеансе Android Auto
  (лабораторный телефон); сеанс теперь идёт, с навигацией, «сейчас играет» и предложенным
  экраном приборки.
- **Install из исходников** («Code → Download ZIP» вместо `mst2-cluster-nav-<версия>.zip` из
  релиза) останавливался только с «MANIFEST is missing». Теперь он перечисляет, что лежит в
  папке, и говорит скачать пакет в разделе Assets релиза; карта с полным пакетом важнее карты с
  исходниками или старой копией.

## 1.1.0

### Новое
- **Android Auto на приборке:** штатные стрелки и полосы, «сейчас играет» и карта телефона в виде
  «Навигация» (перекодируется собственным видеокодером блока) — через возможности приёмника
  Android Auto, которые блок имеет, но не предлагает телефону.
- **«Сейчас играет» с телефона** в медиа-виде приборки, CarPlay и Android Auto, с обложками; на
  паузе скрывается.
- **Круговые развязки Android Auto** — с правильной стороной движения и съездом по пройденному
  углу (или по номеру съезда).
- **Отдаление карты CarPlay** (Green Engineering Menu, по умолчанию выключено): в 1,25× или 1,5×
  больше карты вокруг машины. Телефон рисует карту приборки крупнее, блок декодирует её,
  уменьшает сглаживающим (билинейным) фильтром и кодирует для приборки на 5 Мбит/с — с той же
  скоростью, что собственное видео приборки.
- **История событий, которая переживает перезагрузку:** каждый «страйк» и выключение
  сторожа-самопочинки, каждые Install, Enable, Disable и пробный старт пишутся в
  `/tsd/var/mst2-cluster-nav/history`. Status показывает последние пять, Save diagnostics — все.
- Журнал мода записывает путь звонков CarPlay (только состояния связи и звонка — никаких имён и
  номеров).

### Исправлено
- **При отдалении буквы «рассыпались», а внизу была зелёная полоса.** 2D-ядро графики блока при
  масштабировании через системный композитор берёт ближайший пиксель (фильтр 1×1 — тонкие штрихи
  пропадают целыми строками и столбцами); теперь уменьшение идёт на процессоре нормальным
  фильтром. У перекодированного потока не было ограничения скорости, и большие ключевые кадры
  могли обрезаться по пути к приборке — тогда пропадает низ картинки; теперь поток ограничен,
  как штатный.
- **Отдаление не доходило до приборки** в первых сборках 1.1: композитор отказывал каждому кадру.
- **Блок мог перезагрузиться сразу после старта** (3 старта из 7 на mQbEMU с первыми
  сборками 1.1): фоновый поток мода стартовал раньше, чем был готов распределитель памяти блока.
  Теперь он стартует с первым телефоном.
- **Мод слишком охотно «выключался сам».** Сторож выключал мод после двух подряд стартов HMI в
  пределах 3 минут после старта, который не успел «устояться», — хватало двух быстрых
  выключений-включений. Теперь нужно три подряд (у сторожей нативных частей так же); настоящий
  цикл перезагрузок доходит до этого за минуты.
- **Status показывал старую причину** после пробного старта («did not settle twice»), и
  закончившийся пробный старт выглядел как новое самовыключение. Теперь «Arm trial» стирает
  старую причину; история её хранит.
- Встроенная навигация всегда главнее: пока она ведёт, мод ничего не пишет, и «выключение» мода
  больше не стирает её маршрут.
- Неизвестные время и расстояние до цели остаются пустыми, а не 0:00 / 0.0 км.
- Видео приборки: старт с самого свежего ключевого кадра, безопасная передача между сессиями
  телефона, аварийный выключатель останавливает его сразу.
- Install проверяет каждый файл на карте (размер и контрольная сумма) и формат каждой
  библиотеки до любых изменений, обновляет резервные копии после обновления прошивки и
  сверяет каждый переписанный файл построчно.
- Ведение заканчивается через 120 с без статуса от телефона; название улицы меняется только
  когда известен следующий манёвр.

### Убрано
- Демо-ведение и повторный пункт меню «Phone map».

### Ещё не подтверждено в машине
Новое уменьшение для отдаления и части Android Auto разработаны и проверены на mQbEMU и по коду
прошивки блока; отчёты из машин приветствуются (GEM → Save diagnostics).
