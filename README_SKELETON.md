# Skeleton ESP — всё взято из дампа

Реализован рабочий **скелет (Skeleton ESP)** для оверлея. Все офсеты и структуры
взяты напрямую из дампа игры (`dump 1.7z` → `dump.cs`, заголовки в `dump 2.7z`).

## Откуда берутся кости (цепочка из dump.cs, Assembly-CSharp)

```
PlayerManager.Txa                              : qL                @ 0x248
qL.<HYD>k__BackingField                        : PlayerModelInfo   @ 0x10
PlayerModelInfo.characterAnimation             : CharacterAnimation @ 0x60
CharacterAnimation.ragdoll                     : Ragdoll           @ 0x38
Ragdoll.cGP                                    : List<BodyPart>    @ 0xA0   (основной источник)
Ragdoll.cGw (ключи Dictionary<Transform,LocalTRS>)                 @ 0xA8   (запасной источник)
Ragdoll.BodyPart.transform                     : Transform         @ 0x10
```

Классы/офсеты проверены по дампу:
`PlayerManager`, `qL`, `PlayerModelInfo`, `CharacterAnimation`, `Ragdoll`,
`Ragdoll.BodyPart`, `Ragdoll.LocalTRS`.

Managed-структуры IL2CPP, используемые при чтении:
- `List<T>`: `_items` @ 0x10, `_size` @ 0x18 (уже использовались в проекте);
- массив: `length` @ 0x18, первый элемент @ 0x20;
- `Dictionary<K,V>`: `_entries` @ 0x18, `_count` @ 0x20,
  `Entry{hashCode@0x0, next@0x4, key@0x8, value@0x10}`, размер entry 0x38.

## Как строится скелет

1. Для каждого игрока резолвится список костей (managed `Transform` каждой кости)
   с кэшем на ~3 сек и валидацией: кости обязаны лежать рядом с позицией игрока.
2. Мировая позиция каждой кости считается через существующий движок
   `TransformHierarchy` (native transform + 0x38/0x40 + матрицы/родители).
3. Рёбра скелета строятся из **реальных родительских индексов** трансформов:
   кость соединяется с ближайшим предком, который тоже есть в наборе костей.
   Поэтому лишних мостиков/помощников не рисуется, а топология не хардкодится.
4. Проекция на экран — тем же кодом, что и боксы (матрица камеры либо
   transform-камера, оба режима поддержаны).

## Что изменено в коде

- `jni/src/game_offsets.h` — офсеты цепочки костей из дампа.
- `jni/include/game.h` — в `EspBox` добавлены поля скелета
  (`bones`, `bone_valid`, `bone_edges`, счетчики, `ESP_MAX_BONES = 48`),
  `esp_get_boxes(w, h, collect_skeleton)`.
- `jni/src/game.cpp` — резолвер костей, кэш, сбор скелета и рёбер,
  оппортунистический авто-поиск разметки иерархии трансформов (ускоряет
  чтение позиций костей), очистка кэша в `esp_reset()`.
- `jni/src/main.cpp` — отрисовка скелета в `DrawEspOverlay()` (линии + точки
  суставов, цвет `cfg::esp::skeleton_col`, толщина общая `esp_thick`);
  скелет включён в условие рендера (работает и без включённых боксов).

Пункт меню «Скелет» с выбором цвета уже был в разделе визуалов — теперь он
реально рисует скелет.

## Сборка

```bash
ndk-build        # в корне проекта (рядом с jni/)
```

Бинарник (arm64-v8a) появится в `libs/arm64-v8a/xvcen.sh`.
