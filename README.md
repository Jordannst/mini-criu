# mini-criu

`mini-criu` adalah project berbasis C yang terinspirasi oleh CRIU (Checkpoint/Restore In Userspace) untuk mengeksplorasi konsep checkpoint dan restore proses di Linux/WSL melalui antarmuka CLI yang terstruktur.

Project ini difokuskan pada eksperimen terarah terhadap mekanisme userspace seperti `ptrace`, pembacaan `/proc/<pid>/maps`, akses `/proc/<pid>/mem`, pengambilan register, dan penyimpanan state sederhana ke dalam direktori checkpoint. Fase saat ini sudah mencakup attach ke target, sinkronisasi stop dengan `waitpid`, pencatatan register CPU awal, parsing peta memori, dan dump byte mentah untuk region memori terpilih. Implementasi restore penuh masih belum selesai.

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

Saat ini command `freeze` sudah dapat melakukan attach ke target, menunggu target berhenti, mengambil register CPU, dan menulis data awal checkpoint. Command `dump-memory` sudah dapat mem-parse `/proc/<pid>/maps`, membaca byte mentah dari region memori terpilih, lalu menulis `mem.meta` dan `mem.dump`. Command `restore` masih berada pada tahap fondasi implementasi.

## Batasan Utama

Cakupan `mini-criu` saat ini dibatasi pada kasus yang sengaja dibuat sederhana:

- satu proses
- target single-threaded
- belum mencakup restore socket
- belum mencakup restore file descriptor terbuka
- belum mengimplementasikan alur checkpoint/restore penuh seperti CRIU

## Roadmap

Langkah pengembangan berikutnya yang direncanakan:

1. merapikan konsistensi snapshot antara `freeze` dan `dump-memory`
2. memperkaya format metadata checkpoint agar lebih mudah dipakai saat restore
3. membangun alur restore minimal untuk skenario yang terbatas
4. mengevaluasi validasi tambahan untuk region yang gagal didump atau berubah saat proses berjalan kembali
