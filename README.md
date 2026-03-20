# mini-criu

`mini-criu` adalah proyek eksplorasi checkpoint/restore proses Linux berbasis C dengan antarmuka CLI. Proyek ini terinspirasi dari CRIU, tetapi dibuat dalam bentuk yang lebih kecil, lebih mudah dibaca, dan lebih fokus pada pemahaman mekanisme inti seperti `ptrace`, pembacaan metadata memori, dump register, dump memori mentah, dan percobaan restore parsial.

Fokus utama `mini-criu` adalah:

- memilih proses target
- membuat checkpoint proses
- menyimpan metadata dan isi memori penting
- memuat checkpoint kembali
- menyiapkan alur restore parsial untuk kebutuhan eksplorasi dan diagnosis

## Gambaran Umum

Saat ini `mini-criu` sudah dapat membuat checkpoint yang nyata dan menjalankan alur restore eksperimental secara parsial. Alur restore tersebut sudah mencakup pemuatan checkpoint, penyiapan target restore, injeksi register awal, write-back sebagian memori, resume terkontrol, dan diagnostik yang cukup rinci saat restore belum berhasil berjalan stabil.

Proyek ini belum ditujukan sebagai pengganti CRIU dan belum mencapai restore proses penuh yang stabil.

## Kemampuan Saat Ini

`mini-criu` saat ini dapat:

- memilih target dengan `set-target <pid>`
- melakukan freeze proses dan mengambil snapshot register
- membuat artefak checkpoint:
  - `checkpoint.info`
  - `regs.dump`
  - `mem.meta`
  - `mem.dump`
- menjaga konsistensi snapshot antara `freeze` dan `dump-memory`
- membaca metadata region memori dari `/proc/<pid>/maps`
- membaca isi memori mentah dari `/proc/<pid>/mem`
- memuat dan memvalidasi direktori checkpoint
- membangun rencana restore dari metadata checkpoint
- membuat proses target restore sementara
- menerapkan register awal ke target restore
- menyiapkan sebagian mapping file-backed dan write-back memori
- menjalankan resume eksperimen secara terkontrol
- menampilkan diagnostik restore yang cukup rinci

## Alur Restore Saat Ini

Perintah `restore <checkpoint_dir>` saat ini bekerja sebagai alur restore parsial:

1. memvalidasi direktori checkpoint dan file yang dibutuhkan
2. memuat register dan metadata memori
3. membangun rencana mapping/write-back
4. membuat target restore sementara
5. menerapkan register awal
6. menyiapkan region executable dan file-backed tertentu
7. menulis kembali sebagian data memori
8. mencoba resume secara terkontrol
9. menampilkan diagnostik saat target berhenti lagi atau gagal

Alur ini berguna untuk melihat mekanisme restore dan sumber kegagalannya, tetapi belum bisa dianggap sebagai restore penuh.

## Keterbatasan Saat Ini

Beberapa keterbatasan penting yang masih ada:

- restore masih parsial dan belum stabil
- proses belum berhasil dihidupkan kembali secara penuh
- inkonsistensi stack/control-flow setelah resume masih menjadi hambatan utama
- restore file descriptor belum ada
- restore socket belum ada
- restore multithreading belum ada
- perilaku restore masih bersifat eksploratif, belum production-grade

## Build

Bangun CLI dan target contoh dengan:

```bash
make
```

Hasil build utama:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

Untuk membersihkan hasil build:

```bash
make clean
```

## Menjalankan Program

Cara paling mudah:

```bash
./run-mini-criu
```

Atau langsung:

```bash
./build/mini-criu
```

Untuk menjalankan satu command tanpa masuk shell interaktif:

```bash
./build/mini-criu help
./build/mini-criu status
./build/mini-criu restore checkpoints/contoh
```

## Perintah CLI

- `help`
- `status`
- `set-target <pid>`
- `freeze`
- `dump-memory`
- `restore <checkpoint_dir>`
- `clear`
- `/clear`
- `exit`

## Contoh Alur Penggunaan

Jalankan target uji di terminal lain:

```bash
./build/targets/cpu_bound_target
```

Jalankan CLI:

```bash
./build/mini-criu
```

Contoh alur checkpoint:

```text
mini-criu> set-target 12345
mini-criu> status
mini-criu> freeze
mini-criu> dump-memory
mini-criu> exit
```

Contoh restore:

```bash
./build/mini-criu restore checkpoints/checkpoint-pid-12345-YYYYMMDD-HHMMSS
```

## Isi Direktori Checkpoint

Direktori checkpoint umumnya berisi:

- `checkpoint.info` untuk metadata umum checkpoint
- `regs.dump` untuk snapshot register
- `mem.meta` untuk metadata region memori dan layout dump
- `mem.dump` untuk data memori mentah dari region yang dipilih

## Struktur Proyek

- `src/main.c` dan `src/cli.c` untuk entry point dan command CLI
- `src/freeze.c` untuk freeze target dan dump register
- `src/memory_dump.c` untuk metadata memori dan dump memori mentah
- `src/restore.c` untuk pemuatan checkpoint, restore parsial, resume eksperimen, dan diagnostik
- `src/utils.c` untuk helper bersama
- `include/mini_criu.h` untuk deklarasi umum
- `targets/` untuk proses target sederhana saat pengujian

## Status Proyek

`mini-criu` sudah mampu menunjukkan alur checkpoint yang nyata dan alur restore parsial yang cukup informatif untuk dianalisis. Bagian checkpoint sudah konkret dan dapat dipakai, sedangkan bagian restore masih berfokus pada penyiapan, percobaan resume, dan diagnosis titik gagal yang tersisa.
