# VortexForge

Универсальный десктоп-инструмент для моддеров и мувимейкеров игр на движке **Source** (CS:GO, CS:S, Team Fortress 2, Garry's Mod) с базовой поддержкой инспекции ресурсов **Source 2**.

## Возможности
- **Графический интерфейс (Dear ImGui + GLFW)** и **CLI-режим** из командной строки.
- **Файлы и очередь:** перетаскивание файлов/папок (Drag & Drop), фильтрация, предпросмотр метаданных `.vtf`, `.vmf`, `.vmt`, `.dem`, `.vpk`, а также ресурсов Source 2 (`.vtex_c`, `.vmat_c`, `.vmdl_c`, `.vmap_c`).
- **Карты и сущности (`.vmf`):** экспорт сущностей в JSON и CSV, статистика карты, экспорт 3D-маркеров в `.obj`, проверка отсутствующих зависимостей.
- **Текстуры и материалы (`.vtf` / `.vmt`):** просмотр заголовков VTF, конвертация, нормализация путей и параметров в VMT, генерация базовых VMT.
- **Демки и утилиты:** разбор заголовков `.dem`, генерация `.cfg` с `demo_gototick` и таймкодов, проверка путей (Lint), поиск неиспользуемых ассетов, сборка паков с `manifest.json`.
- **Мультиязычность (RU / EN)**, светлая/тёмная тема и проверка обновлений через GitHub Releases.

## Сборка из исходников (Windows)
```powershell
cmake -S . -B build_gui -DBUILD_WITH_IMGUI=ON -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build_gui --config Release