# Changelog

**English** · [Русский](#русский)

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
