# mini-criu

`mini-criu` adalah prototipe akademik kecil yang terinspirasi oleh CRIU (Checkpoint/Restore In Userspace). Proyek ini ditulis dalam bahasa C dan ditujukan untuk Linux/WSL sebagai eksplorasi konsep checkpoint/restore proses pada level rendah.

Fase saat ini berfokus pada scaffold CLI yang rapi dan struktur proyek yang realistis, bukan implementasi penuh. Basis kode ini sudah disiapkan untuk pengembangan lanjutan terkait `ptrace`, pengambilan register, `/proc/<pid>/maps`, `/proc/<pid>/mem`, dan keluaran direktori checkpoint sederhana.

## Status Saat Ini

Repositori saat ini sudah menyediakan:

- CLI `mini-criu` yang dapat dikompilasi
- command loop interaktif
- command placeholder untuk pengembangan checkpoint/restore berikutnya
- header bersama dan utilitas umum
- dua program target dummy single-threaded untuk pengujian
- scaffold direktori checkpoint sederhana untuk output metadata

Yang belum diimplementasikan:

- logika attach/stop nyata menggunakan `ptrace`
- pengambilan dan pemulihan register
- parsing memory map
- dump memori proses dari `/proc/<pid>/mem`
- alur restore yang nyata
- dukungan untuk file descriptor, socket, atau proses multithread

## Struktur Folder

```text
mini-criu/
├── checkpoints/              # Folder checkpoint yang dihasilkan
├── docs/                     # Catatan desain dan dokumentasi lanjutan
├── include/
│   └── mini_criu.h           # Tipe bersama dan deklarasi fungsi
├── src/
│   ├── main.c                # Entry point program
│   ├── cli.c                 # Parser command dan loop REPL
│   ├── freeze.c              # Stub logika freeze/checkpoint
│   ├── memory_dump.c         # Scaffold dump memori
│   ├── restore.c             # Scaffold restore
│   └── utils.c               # Helper umum
├── targets/
│   ├── cpu_bound_target.c    # Proses target sederhana yang intensif CPU
│   └── memory_bound_target.c # Proses target sederhana yang intensif memori
├── Makefile
└── README.md
```

## Build

Untuk membangun CLI dan target dummy:

```bash
make
```

Artifact hasil build akan berada di dalam `build/`:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

Untuk membersihkan hasil build:

```bash
make clean
```

## Menjalankan Program

Menjalankan CLI interaktif:

```bash
./build/mini-criu
```

Menjalankan satu command tanpa masuk ke REPL:

```bash
./build/mini-criu status
./build/mini-criu help
./build/mini-criu restore checkpoints/example
```

### Command Interaktif

- `help`
- `status`
- `set-target <pid>`
- `freeze`
- `dump-memory`
- `restore <checkpoint_dir>`
- `exit`

### Contoh Alur Penggunaan

1. Build proyek dengan `make`
2. Jalankan target dummy di terminal lain:

```bash
./build/targets/cpu_bound_target
```

3. Jalankan CLI:

```bash
./build/mini-criu
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

Command `freeze`, `dump-memory`, dan `restore` saat ini masih berupa scaffold. Command-command ini sudah melakukan validasi input, menampilkan progres dasar, dan untuk `dump-memory` akan membuat direktori checkpoint sederhana yang berisi metadata.

## Roadmap

Urutan implementasi yang disarankan untuk fase berikutnya:

1. Implementasikan attach dan stop/wait nyata berbasis `ptrace` di `freeze.c`
2. Tambahkan pengambilan register dan serialisasi datanya
3. Parse `/proc/<pid>/maps` ke representasi in-memory sederhana
4. Dump region memori terpilih dari `/proc/<pid>/mem`
5. Tentukan format metadata checkpoint
6. Implementasikan jalur restore minimal untuk kasus target yang sangat terbatas
7. Tambahkan dokumentasi di `docs/` untuk format checkpoint dan keputusan desain

## Cakupan dan Batasan

Proyek ini sengaja dibuat dengan cakupan kecil:

- hanya satu proses
- hanya target single-threaded
- tidak mendukung restore socket
- tidak mendukung restore file descriptor terbuka
- prototipe edukatif, bukan CRIU production-grade

Batasan tersebut memang disengaja. Tujuannya adalah membangun proyek sistem yang mudah dipahami dan bisa dikembangkan secara bertahap, bukan membuat sistem checkpoint container/runtime yang lengkap.
