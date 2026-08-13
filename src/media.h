#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace media {

// Результат анализа входного файла (ffprobe).
struct Probe {
    bool ok = false;
    std::string error;

    std::string format_name;   // "flac", "mp3", "matroska,webm", ...
    std::string codec_name;    // аудио-кодек первого аудиопотока
    int channels = 0;
    int sample_rate = 0;
    int bits_per_sample = 0;   // 0, если неизвестно (mp3 и т.п.) — далее 16
    double duration = 0;       // секунды
    uint64_t size = 0;
    bool has_video = false;    // в файле есть видеопоток — оптимизация пропускается

    // true, если аудиокодек lossless. Пустой codec_name (неизвестный кодек)
    // трактуется как lossless, чтобы не блокировать обработку.
    bool is_lossless() const;

    // Текстовые теги из контейнера (format.tags): ключ -> значения.
    std::map<std::string, std::vector<std::string>> tags;
};

// Возвращает true, если аудиокодек lossless (по имени codec_name ffprobe).
bool codec_is_lossless(const std::string& codec_name);

// Анализ файла через ffprobe.exe. ffprobe — путь к ffprobe (пусто = поиск в PATH).
Probe probe_file(const std::string& path, const std::string& ffprobe);

// Декод в эталонный WAV через ffmpeg.exe. Битность берётся из пробы
// (16/24/32; не более глубины исходника). Путь к ffmpeg — как в конфиге.
// Возвращает true при успехе; err — текст ошибки.
bool decode_to_wav(const std::string& input, const std::string& output_wav,
                   const std::string& ffmpeg, int bits, std::string* err);

// Путь к ffprobe: bin/ffmpeg/ffprobe.exe (рядом с exe) или из PATH.
std::string find_ffprobe();
std::string find_ffmpeg();

// Сравнение PCM-данных двух WAV-файлов (по содержимому data-чанка, заголовки
// игнорируются). Возвращает true при побитовом совпадении; err — текст ошибки.
bool wav_pcm_equal(const std::string& a, const std::string& b, std::string* err);

}  // namespace media
