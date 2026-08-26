# USBLiter8 BootSurreal

Автономная надстройка над `usbliter8`: после успешного перехода Apple DFU в PWNED DFU плата сама отправляет заранее встроенный `*.boot` payload, без `usbliter8ctl` и без ноутбука.

Проект берет исходники `FogboundSloth25/usbliter8bootsurreal` при сборке, добавляет post-exploitation DFU sender и вшивает payload в XIP flash RP2350 через GNU assembler `.incbin`.

## Что происходит

```text
iPhone/iPad DFU
      ↓
RP2350 USB host
      ↓
usbliter8 exploit
      ↓
PWNED DFU
      ↓
DFU_DNLOAD по 0x800 байт
      ↓
нулевой DFU_DNLOAD
      ↓
custom BOOT (request 8)
      ↓
DFU_ABORT
      ↓
boot payload
```

Команды post-exploitation соответствуют host-side `usbliter8ctl`: `DFU_DNLOAD=1`, `CUSTOM_BOOT=8`, `DFU_ABORT=4`, размер блока `0x800`.

## Bootfiles

В ZIP оставлены файлы из твоего `boot.zip`:

```text
bootfiles/
└── iPhone11,2/
    ├── iBSS.patch
    ├── 16.5.1/
    │   └── iBSS.boot
    └── 0x001c25323433002e.txt
```

В firmware сейчас встраиваются только `*.boot`; `.patch` и marker остаются для reference/provenance.

## Сборка Fedora 44

```bash
chmod +x build.sh
./build.sh
```

`build.sh` сам ставит native зависимости через `dnf`, скачивает Pico SDK 2.2.0, пытается скачать Arm GNU Toolchain 15.2.Rel1, скачивает upstream fork, просит выбрать плату и собирает UF2.

Поддерживаемые варианты:

```text
waveshare_rp2350_usb_a
waveshare_rp2350_zero
pimoroni_tiny2350
pico2
adafruit_feather_rp2040
pico
```

RP2350 предпочтительнее: upstream отдельно предупреждает о меньшей стабильности RP2040 и отсутствии A13 на RP2040.

## Ограничения текущей v1.0

Если в `bootfiles/` несколько `*.boot`, firmware пока берет первый файл в лексикографическом порядке. Автоматического сопоставления `iPhone model → iOS version → bootfile` еще нет.

`iBSS.patch` автоматически не применяется: ожидается, что `*.boot` уже является конечным payload, который надо отправлять в PWNED DFU.

## Источники

- `https://github.com/FogboundSloth25/usbliter8bootsurreal`
- `https://github.com/ahmadkamal09999-tech/usbliter8`
- `https://github.com/pwnerblu/surrealra1n`
