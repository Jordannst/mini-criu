# mini-criu

`mini-criu` adalah project berbasis C yang terinspirasi oleh CRIU (Checkpoint/Restore In Userspace) untuk mengeksplorasi konsep checkpoint dan restore proses di Linux/WSL melalui antarmuka CLI yang terstruktur.

Project ini difokuskan pada eksperimen terarah terhadap mekanisme userspace seperti `ptrace`, pembacaan `/proc/<pid>/maps`, akses `/proc/<pid>/mem`, pengambilan register, dan penyimpanan state sederhana ke dalam direktori checkpoint. Saat ini `freeze` membuka satu event snapshot, menahan target tetap berhenti, lalu `dump-memory` melanjutkan snapshot yang sama untuk menulis metadata peta memori dan dump byte mentah. Implementasi restore penuh masih belum selesai.

## Tujuan Project

`mini-criu` dibangun untuk:

- menyediakan fondasi sederhana untuk eksperimen checkpoint/restore proses di Linux
- memisahkan komponen CLI, utilitas, freeze, dump memori, dan restore ke modul yang jelas
- menyediakan target dummy yang mudah diuji untuk pengembangan dan observasi perilaku proses

## Struktur Folder

```text
mini-criu/
├── checkpoints/              # Folder checkpoint yang dihasilkan
├── docs/                     # Catatan desain dan dokumentasi tambahan
├── include/
│   └── mini_criu.h           # Tipe bersama dan deklarasi fungsi
├── src/
│   ├── main.c                # Entry point program
│   ├── cli.c                 # Parser command dan loop REPL
│   ├── freeze.c              # Fondasi logika freeze/checkpoint
│   ├── memory_dump.c         # Fondasi dump memori
│   ├── restore.c             # Fondasi restore
│   └── utils.c               # Helper umum
├── targets/
│   ├── cpu_bound_target.c    # Proses target sederhana yang intensif CPU
│   └── memory_bound_target.c # Proses target sederhana yang intensif memori
├── Makefile
├── run-mini-criu             # Launcher untuk menjalankan CLI utama
└── README.md
```

## Build

Untuk membangun CLI dan program target:

```bash
make
```

Hasil build akan tersedia di dalam folder `build/`:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

Untuk membersihkan hasil build:

```bash
make clean
```

## Menjalankan Program

Cara paling praktis untuk menjalankan CLI:

```bash
./run-mini-criu
```

Launcher ini akan menjalankan `make` otomatis jika binary utama belum tersedia.

Menjalankan CLI langsung dari hasil build:

```bash
./build/mini-criu
```

Menjalankan satu command tanpa masuk ke mode interaktif:

```bash
./run-mini-criu status
./run-mini-criu help
./run-mini-criu restore checkpoints/example
```

### Command yang Tersedia

- `help`
- `status`
- `clear`
- `/clear`
- `set-target <pid>`
- `freeze`
- `dump-memory`
- `restore <checkpoint_dir>`
- `exit`

### Contoh Alur Penggunaan

1. Build project dengan `make`
2. Jalankan target dummy di terminal lain:

```bash
./build/targets/cpu_bound_target
```

3. Jalankan CLI:

```bash
./run-mini-criu
```

4. Di dalam CLI:

```text
mini-criu> set-target 12345
mini-criu> status
mini-criu> freeze
mini-criu> dump-memory
mini-criu> restore checkpoints/checkpoint-pid-12345-YYYYMMDD-HHMMSS
mini-criu> exit
```

Saat ini command `freeze` membuka snapshot, mengambil register CPU, menulis `regs.dump`, lalu menahan target tetap berhenti selama sesi CLI yang sama. Command `dump-memory` melanjutkan snapshot aktif tersebut, mem-parse `/proc/<pid>/maps`, membaca byte mentah dari region memori terpilih, menulis `mem.meta` dan `mem.dump`, lalu melepaskan target kembali. Command `restore` sekarang dapat memuat checkpoint yang sudah ada, memvalidasi file utama, membaca metadata/register, dan membuat kerangka target restore sementara tanpa mengeksekusi restore penuh.

## Batasan Utama

Cakupan `mini-criu` saat ini dibatasi pada kasus yang sengaja dibuat sederhana:

- satu proses
- target single-threaded
- belum mencakup restore socket
- belum mencakup restore file descriptor terbuka
- belum mengimplementasikan alur checkpoint/restore penuh seperti CRIU

## Roadmap

Langkah pengembangan berikutnya yang direncanakan:

1. memperkaya format metadata checkpoint agar lebih mudah dipakai saat restore
2. membangun alur restore minimal untuk skenario yang terbatas
3. mengevaluasi validasi tambahan untuk region yang gagal didump
4. meninjau batasan snapshot untuk kasus proses yang lebih kompleks
