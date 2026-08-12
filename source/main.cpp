#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <archive.h>
#include <archive_entry.h>
#include <3ds/ndsp/ndsp.h>
#include <tremor/ivorbisfile.h>
#include "imgui/imgui.h"
#include "imgui/imgui_sw.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <charconv>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <functional>
#include <iterator>
#include <limits>
#include <ranges>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>
static_assert(CHAR_BIT == 8, "Byte-oriented buffer maths assumes 8-bit bytes.");
static_assert(sizeof(s16) == 2, "PCM16 framing assumes a 2-byte sample.");
static_assert(sizeof(u16) == 2, "RGB565 texel packing assumes a 2-byte texel.");
namespace {
  namespace cfg {
    template < std::size_t N >
      struct StaticString {
        std::array < char, N + 1 > chars {};
        [
          [nodiscard]
        ] constexpr
        const char * c_str() const noexcept {
            return chars.data();
          }
          [
            [nodiscard]
          ] constexpr std::string_view view() const noexcept {
              return {
                chars.data(),
                N
              };
            }
            [
              [nodiscard]
            ] static constexpr std::size_t size() noexcept {
              return N;
            }
      };
    template < std::size_t N > [
      [nodiscard]
    ] consteval StaticString < N - 1 > lit(const char( & source)[N]) noexcept {
      StaticString < N - 1 > out;
      for (std::size_t i = 0; i + 1 < N; ++i) {
        out.chars[i] = source[i];
      }
      return out;
    }
    template < std::size_t A, std::size_t B > [
      [nodiscard]
    ] consteval StaticString < A + B > operator + (const StaticString < A > & a,
      const StaticString < B > & b) noexcept {
      StaticString < A + B > out;
      for (std::size_t i = 0; i < A; ++i) {
        out.chars[i] = a.chars[i];
      }
      for (std::size_t i = 0; i < B; ++i) {
        out.chars[A + i] = b.chars[i];
      }
      return out;
    }
    inline constexpr
    const char * USER_AGENT = "CTGP-7-Mod-Manager/3.0";
    inline constexpr
    const char * API_V10_INDEX = "https://gamebanana.com/apiv10/Mod/Index";
    inline constexpr
    const char * API_CORE_DATA = "https://api.gamebanana.com/Core/Item/Data";
    inline constexpr
    const char * CA_BUNDLE_PATH = "romfs:/cacert.pem";
    inline constexpr
    const char * DOWNLOAD_BASE = "https://gamebanana.com/dl/";
    inline constexpr std::size_t SOC_ALIGN = 0x1000;
    inline constexpr std::size_t SOC_BUFFERSIZE = 0x100000;
    inline constexpr std::size_t MAX_RESPONSE_SIZE = 512 * 1024;
    inline constexpr int MAX_FETCH_ATTEMPTS = 4;
    inline constexpr u64 RETRY_BASE_DELAY_NS = 500'000'000ULL;
    inline constexpr u64 THROTTLE_DELAY_NS = 1'500'000'000ULL;
    inline constexpr std::size_t FETCH_WORKERS = 6;
    inline constexpr std::size_t WORKER_STACK_SIZE = 128 * 1024;
    inline constexpr int WORKER_PRIORITY = 0x3F;
    inline constexpr int AUDIO_PRIORITY = 0x30;
    inline constexpr int ANY_CORE = -2;
    inline constexpr int INDEX_PER_PAGE = 50;
    inline constexpr int MAX_PAGES_PER_CAT = 200;
    inline constexpr std::size_t CORE_BATCH_SIZE = 50;
    inline constexpr std::size_t RESPONSE_RESERVE = 64 * 1024;
    inline constexpr std::array < int, 18 > CATEGORIES {35931, 10605, 35932, 35943, 35933, 35935, 35937, 35938, 35939, 35941, 35942, 35944, 35946, 35947, 35945, 35940, 35934, 35936};
    inline constexpr auto BASE_DIR = lit("sdmc:/");
    inline constexpr auto APP_DIR = BASE_DIR + lit("3ds/CTGP-7-Mod-Manager/");
    inline constexpr auto CACHE_DIR = APP_DIR + lit("cache/");
    inline constexpr auto LISTS_DIR = CACHE_DIR + lit("lists/");
    inline constexpr auto THUMB_DIR = CACHE_DIR + lit("images/");
    inline constexpr auto CTGP7_DIR = BASE_DIR + lit("CTGP-7/MyStuff/Characters/");
    inline constexpr auto MOD_LIST_FILE = LISTS_DIR + lit("modlist.json");
    inline constexpr auto BY_NAME_FILE = LISTS_DIR + lit("byname.json");
    inline constexpr auto BY_UPDATED_FILE = LISTS_DIR + lit("byupdated.json");
    inline constexpr auto INSTALLED_FILE = APP_DIR + lit("installed_mods.json");
    inline constexpr auto INSTALLED_TMP = APP_DIR + lit("installed_mods.json.tmp");
    inline constexpr auto DOWNLOAD_TMP = CACHE_DIR + lit("download.tmp");
    inline constexpr int AUDIO_BUF_SAMPLES = 4096;
    inline constexpr int AUDIO_MAX_CHANNELS = 2;
    inline constexpr int AUDIO_VOL = 0x50;
    inline constexpr std::size_t AUDIO_BUF_BYTES = static_cast < std::size_t > (AUDIO_BUF_SAMPLES) * static_cast < std::size_t > (AUDIO_MAX_CHANNELS) * sizeof(s16); static_assert(AUDIO_MAX_CHANNELS >= 2, "play() clamps to 2 channels; wave buffers must be sized for that.");
    inline constexpr float TOP_W = 400.0f;
    inline constexpr float TOP_H = 240.0f;
    inline constexpr float BOT_W = 320.0f;
    inline constexpr float BOT_H = 240.0f;
    inline constexpr int GRID_COLS = 3;
    inline constexpr int GRID_ROWS = 2;
    inline constexpr int CARDS_PER_PAGE = GRID_COLS * GRID_ROWS;
    inline constexpr float CELL_W = TOP_W / GRID_COLS;
    inline constexpr float CELL_H = TOP_H / GRID_ROWS;
    inline constexpr float CARD_MARGIN = 2.0f;
    inline constexpr float CARD_BORDER = 2.0f;
    inline constexpr float CARD_W = CELL_W - CARD_MARGIN * 2.0f;
    inline constexpr float CARD_H = CELL_H - CARD_MARGIN * 2.0f;
    inline constexpr float CONTENT_W = CARD_W - CARD_BORDER * 2.0f;
    inline constexpr float CONTENT_H = CARD_H - CARD_BORDER * 2.0f;
    inline constexpr float TEXT_MAX_W = CONTENT_W - 4.0f;
    inline constexpr float THUMB_H = 70.0f;
    inline constexpr float NAME_Y = 71.0f;
    inline constexpr float NAME_PX = 14.0f;
    inline constexpr float AUTHOR_Y = 85.0f;
    inline constexpr float AUTHOR_PX = 11.0f;
    inline constexpr float STATUS_Y = 97.0f;
    inline constexpr float STATUS_PX = 14.0f;
    inline constexpr float MSG_PX = 18.0f;
    inline constexpr float BTN_W = 280.0f;
    inline constexpr float BTN_X = (BOT_W - BTN_W) * 0.5f;
    inline constexpr float ACTION_BTN_H = 44.0f;
    inline constexpr float UNINST_BTN_H = 34.0f;
    inline constexpr float SORT_LABEL_Y = 6.0f;
    inline constexpr float SORT_ROW_Y = 26.0f;
    inline constexpr float SEP_Y = 54.0f;
    inline constexpr float ACTION_BTN_Y = 62.0f;
    inline constexpr float UNINST_BTN_Y = 114.0f;
    inline constexpr float COUNTER_Y = 158.0f;
    inline constexpr float HINT1_Y = 190.0f;
    inline constexpr float HINT2_Y = 210.0f;
    inline constexpr float PROG_BAR_Y = 108.0f;
    inline constexpr float PROG_BAR_H = 4.0f;
    inline constexpr float MSG_LINE_Y = 168.0f;
    inline constexpr int MSG_MAX_LINES = 3;
    inline constexpr float BTN_TEXT_PAD = 8.0f;
    inline constexpr float BTN_ROUNDING = 5.0f;
    inline constexpr int BTN_MAX_LINES = 2;
    inline constexpr std::size_t CARD_TEXT_GLYPHS = 2048;
    inline constexpr std::size_t SCRATCH_GLYPHS = 512;
    inline constexpr std::size_t MEASURE_MAX_LEN = 192;
    inline constexpr float NAV_REPEAT_DELAY = 0.35f;
    inline constexpr float NAV_REPEAT_RATE = 0.07f;
    inline constexpr ImGuiWindowFlags SCREEN_WINDOW_FLAGS = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    inline constexpr
    const char * CHPACK_SUFFIX = ".chpack";
    inline constexpr std::size_t INSTALL_NAME_MAX = 128;
    inline constexpr std::size_t INSTALL_MAX_FILES = 64;
    inline constexpr int INSTALL_MAX_ENTRIES = 20000;
    inline constexpr int64_t INSTALL_MAX_FILE_BYTES = 64 * 1024 * 1024;
    inline constexpr int64_t INSTALL_MAX_TOTAL_BYTES = 256 * 1024 * 1024;
    inline constexpr std::size_t ARCHIVE_BLOCK_SIZE = 64 * 1024;
    inline constexpr curl_off_t DOWNLOAD_MAX_BYTES = 64 * 1024 * 1024;
    inline constexpr long DL_CONNECT_TIMEOUT = 20L;
    inline constexpr long DL_LOW_SPEED_LIMIT = 1024L;
    inline constexpr long DL_LOW_SPEED_TIME = 30L;
    inline constexpr std::size_t INSTALL_STACK_SIZE = 256 * 1024;
    inline constexpr s64 INSTALL_IDLE_WAIT_NS = 50'000'000LL;
    inline constexpr int THUMB_IMG_W = 110;
    inline constexpr int THUMB_IMG_H = 62;
    inline constexpr int THUMB_JPEG_QUALITY = 50;
    inline constexpr u16 THUMB_TEX_W = 128;
    inline constexpr u16 THUMB_TEX_H = 64;
    inline constexpr std::size_t THUMB_TEX_BYTES = static_cast < std::size_t > (THUMB_TEX_W) * static_cast < std::size_t > (THUMB_TEX_H) * 2u; static_assert((THUMB_TEX_W & (THUMB_TEX_W - 1)) == 0, "PICA200 requires power-of-two textures."); static_assert((THUMB_TEX_H & (THUMB_TEX_H - 1)) == 0, "PICA200 requires power-of-two textures."); static_assert(THUMB_TEX_W >= THUMB_IMG_W && THUMB_TEX_H >= THUMB_IMG_H, "The texture must be able to hold the cached image.");
    inline constexpr int THUMB_SLOTS = 16;
    inline constexpr int THUMB_WORKERS = 3;
    inline constexpr int THUMB_QUEUE = CARDS_PER_PAGE;
    inline constexpr int THUMB_FAIL_RING = 64;
    static_assert(THUMB_SLOTS > CARDS_PER_PAGE, "Eviction must always find a victim that is not currently on screen.");
    inline constexpr std::size_t THUMB_URL_MAX = 192;
    inline constexpr std::size_t THUMB_MAX_BYTES = 1024 * 1024;
    inline constexpr unsigned THUMB_SRC_MAX_DIM = 4096;
    inline constexpr std::size_t THUMB_SRC_MAX_PIXELS = 2'000'000;
    inline constexpr s64 THUMB_IDLE_WAIT_NS = 50'000'000LL;
    inline constexpr u32 CLR_BG = C2D_Color32(0x15, 0x1D, 0x23, 0xFF);
    inline constexpr u32 CLR_SEL_BG = C2D_Color32(0x2A, 0x3B, 0x47, 0xFF);
    inline constexpr u32 CLR_GOLD = C2D_Color32(0xAB, 0xA0, 0x22, 0xFF);
    inline constexpr u32 CLR_GREEN = C2D_Color32(0x4C, 0xAF, 0x50, 0xFF);
    inline constexpr u32 CLR_AMBER = C2D_Color32(0xFF, 0xC1, 0x07, 0xFF);
    inline constexpr u32 CLR_AUTHOR = C2D_Color32(0x88, 0x88, 0x88, 0xFF);
    inline constexpr u32 CLR_ERROR = C2D_Color32(0xFF, 0x55, 0x55, 0xFF);
    inline constexpr u32 CLR_THUMB = C2D_Color32(0x00, 0x00, 0x00, 0xFF);
    inline constexpr u32 CLR_SEP = CLR_SEL_BG;
    [
      [nodiscard]
    ] constexpr ImVec4 im_color(u8 r, u8 g, u8 b, u8 a = 0xFF) noexcept {
      return ImVec4(static_cast < float > (r) / 255.0f,
        static_cast < float > (g) / 255.0f,
        static_cast < float > (b) / 255.0f,
        static_cast < float > (a) / 255.0f);
    }
    inline constexpr ImVec4 IM_WINDOW_BG = im_color(0x15, 0x1D, 0x23);
    inline constexpr ImVec4 IM_GOLD = im_color(0xAB, 0xA0, 0x22);
    inline constexpr ImVec4 IM_AMBER = im_color(0xFF, 0xC1, 0x07);
    inline constexpr ImVec4 IM_AUTHOR = im_color(0x88, 0x88, 0x88);
    inline constexpr ImVec4 IM_ERROR = im_color(0xFF, 0x55, 0x55);
    inline constexpr ImVec4 IM_BTN_BG = im_color(0x2A, 0x3B, 0x47);
    inline constexpr ImVec4 IM_BTN_HOT = im_color(0x38, 0x4E, 0x5D);
    inline constexpr ImVec4 IM_UNINST_BG = im_color(0x4A, 0x27, 0x27);
    inline constexpr ImVec4 IM_UNINST_HOT = im_color(0x63, 0x34, 0x34);
    inline constexpr ImVec4 IM_UNINST_FG = im_color(0xFF, 0x88, 0x88);
  }
  namespace sys {
    using CtruThread = ::Thread;
    class Mutex {
      public: Mutex() noexcept {
        LightLock_Init( & lock_);
      }
      Mutex(const Mutex & ) = delete;
      Mutex & operator = (const Mutex & ) = delete;
      void lock() noexcept {
        LightLock_Lock( & lock_);
      }
      void unlock() noexcept {
        LightLock_Unlock( & lock_);
      }
      [
        [nodiscard]
      ] bool try_lock() noexcept {
        return LightLock_TryLock( & lock_) == 0;
      }
      private: LightLock lock_ {};
    };
    class RecursiveMutex {
      public: RecursiveMutex() noexcept {
        RecursiveLock_Init( & lock_);
      }
      RecursiveMutex(const RecursiveMutex & ) = delete;
      RecursiveMutex & operator = (const RecursiveMutex & ) = delete;
      void lock() noexcept {
        RecursiveLock_Lock( & lock_);
      }
      void unlock() noexcept {
        RecursiveLock_Unlock( & lock_);
      }
      private: RecursiveLock lock_ {};
    };
    class Event {
      public: explicit Event(bool sticky = true) noexcept {
        LightEvent_Init( & event_, sticky ? RESET_STICKY : RESET_ONESHOT);
      }
      Event(const Event & ) = delete;
      Event & operator = (const Event & ) = delete;
      void clear() noexcept {
        LightEvent_Clear( & event_);
      }
      void signal() noexcept {
        LightEvent_Signal( & event_);
      }
      void wait_for(s64 timeout_ns) noexcept {
        LightEvent_WaitTimeout( & event_, timeout_ns);
      }
      private: LightEvent event_ {};
    };
    class Thread {
      public: Thread() noexcept =
        default;
      Thread(const Thread & ) = delete;
      Thread & operator = (const Thread & ) = delete;
      Thread(Thread && other) noexcept: handle_ {
        std::exchange(other.handle_, nullptr)
      } {}
      Thread & operator = (Thread && other) noexcept {
        if (this != & other) {
          join();
          handle_ = std::exchange(other.handle_, nullptr);
        }
        return * this;
      }
      ~Thread() {
        join();
      }
      [
        [nodiscard]
      ] static Thread spawn(ThreadFunc entry, void * arg, std::size_t stack_size,
        int priority, int core) noexcept {
        return Thread {
          threadCreate(entry, arg, stack_size, priority, core, false)
        };
      }
      void join() noexcept {
        if (handle_ == nullptr) {
          return;
        }
        threadJoin(handle_, U64_MAX);
        threadFree(handle_);
        handle_ = nullptr;
      }
      [
        [nodiscard]
      ] explicit operator bool() const noexcept {
        return handle_ != nullptr;
      }
      private: explicit Thread(CtruThread handle) noexcept: handle_ {
        handle
      } {}
      CtruThread handle_ = nullptr;
    };
    struct FreeDeleter {
      void operator()(void * p) const noexcept {
        std::free(p);
      }
    };
    template < typename T >
      using MallocArray = std::unique_ptr < T[], FreeDeleter > ;
    class LinearBuffer {
      public: LinearBuffer() noexcept =
        default;
      explicit LinearBuffer(std::size_t bytes) noexcept: data_ {
        linearAlloc(bytes)
      } {}
      LinearBuffer(const LinearBuffer & ) = delete;
      LinearBuffer & operator = (const LinearBuffer & ) = delete;
      LinearBuffer(LinearBuffer && other) noexcept: data_ {
        std::exchange(other.data_, nullptr)
      } {}
      LinearBuffer & operator = (LinearBuffer && other) noexcept {
        if (this != & other) {
          reset();
          data_ = std::exchange(other.data_, nullptr);
        }
        return * this;
      }
      ~LinearBuffer() {
        reset();
      }
      void reset() noexcept {
        if (data_ == nullptr) {
          return;
        }
        linearFree(data_);
        data_ = nullptr;
      }
      void swap_with(void * & other) noexcept {
        std::swap(data_, other);
      }
      [
        [nodiscard]
      ] void * get() const noexcept {
        return data_;
      }
      template < typename T > [
        [nodiscard]
      ] T * as() const noexcept {
          return static_cast < T * > (data_);
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return data_ != nullptr;
        }
      private: void * data_ = nullptr;
    };
    class FileHandle {
      public: FileHandle() noexcept =
        default;
      explicit FileHandle(std::FILE * file) noexcept: file_ {
        file
      } {}
      FileHandle(const FileHandle & ) = delete;
      FileHandle & operator = (const FileHandle & ) = delete;
      FileHandle(FileHandle && other) noexcept: file_ {
        std::exchange(other.file_, nullptr)
      } {}
      FileHandle & operator = (FileHandle && other) noexcept {
        if (this != & other) {
          (void) close();
          file_ = std::exchange(other.file_, nullptr);
        }
        return * this;
      }
      ~FileHandle() {
        (void) close();
      }
      bool close() noexcept {
        if (file_ == nullptr) {
          return true;
        }
        const bool ok = std::fclose(file_) == 0;
        file_ = nullptr;
        return ok;
      }
      [
        [nodiscard]
      ] std::FILE * release() noexcept {
        return std::exchange(file_, nullptr);
      }
      [
        [nodiscard]
      ] std::FILE * get() const noexcept {
          return file_;
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return file_ != nullptr;
        }
      private: std::FILE * file_ = nullptr;
    };
    class CurlHandle {
      public: CurlHandle() noexcept: handle_ {
        curl_easy_init()
      } {}
      CurlHandle(const CurlHandle & ) = delete;
      CurlHandle & operator = (const CurlHandle & ) = delete;
      ~CurlHandle() {
        if (handle_ != nullptr) {
          curl_easy_cleanup(handle_);
        }
      }
      [
        [nodiscard]
      ] CURL * get() const noexcept {
          return handle_;
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return handle_ != nullptr;
        }
      private: CURL * handle_ = nullptr;
    };
    class CurlShare {
      public: CurlShare() =
        default;
      CurlShare(const CurlShare & ) = delete;
      CurlShare & operator = (const CurlShare & ) = delete;
      ~CurlShare() {
        reset();
      }
      bool init(curl_lock_function lock_fn, curl_unlock_function unlock_fn) noexcept {
        reset();
        handle_ = curl_share_init();
        if (handle_ == nullptr) {
          return false;
        }
        curl_share_setopt(handle_, CURLSHOPT_LOCKFUNC, lock_fn);
        curl_share_setopt(handle_, CURLSHOPT_UNLOCKFUNC, unlock_fn);
        curl_share_setopt(handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        return true;
      }
      void reset() noexcept {
        if (handle_ == nullptr) {
          return;
        }
        curl_share_cleanup(handle_);
        handle_ = nullptr;
      }
      [
        [nodiscard]
      ] CURLSH * get() const noexcept {
        return handle_;
      }
      private: CURLSH * handle_ = nullptr;
    };
    class JsonRef {
      public: JsonRef() noexcept =
        default;
      explicit JsonRef(json_object * object) noexcept: object_ {
        object
      } {}
      JsonRef(const JsonRef & ) = delete;
      JsonRef & operator = (const JsonRef & ) = delete;
      JsonRef(JsonRef && other) noexcept: object_ {
        std::exchange(other.object_, nullptr)
      } {}
      JsonRef & operator = (JsonRef && other) noexcept {
        if (this != & other) {
          reset(std::exchange(other.object_, nullptr));
        }
        return * this;
      }
      ~JsonRef() {
        reset();
      }
      void reset(json_object * object = nullptr) noexcept {
        if (object_ != nullptr) {
          json_object_put(object_);
        }
        object_ = object;
      }
      [
        [nodiscard]
      ] json_object * release() noexcept {
        return std::exchange(object_, nullptr);
      }
      [
        [nodiscard]
      ] json_object * get() const noexcept {
          return object_;
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return object_ != nullptr;
        }
      private: json_object * object_ = nullptr;
    };
    template < std::invocable Fn >
      class ScopeGuard {
        public: explicit ScopeGuard(Fn action) noexcept: action_ {
          std::move(action)
        } {}
        ScopeGuard(const ScopeGuard & ) = delete;
        ScopeGuard & operator = (const ScopeGuard & ) = delete;
        ~ScopeGuard() {
          if (armed_) {
            action_();
          }
        }
        void dismiss() noexcept {
          armed_ = false;
        }
        private: Fn action_;
        bool armed_ = true;
      };
    template < std::invocable Fn >
      ScopeGuard(Fn) -> ScopeGuard < Fn > ;
  }
  namespace fmt {
    inline void format_placeholder_count_does_not_match_arguments() {}
    template < typename...Args >
      class Spec {
        public: template < std::size_t N >
          consteval Spec(const char( & literal)[N]): text_ {
            literal,
            N - 1
          } {
            std::size_t placeholders = 0;
            for (std::size_t i = 0; i + 1 < N - 1;) {
              const bool doubled = literal[i] == literal[i + 1];
              if ((literal[i] == '{' || literal[i] == '}') && doubled) {
                i += 2;
                continue;
              }
              if (literal[i] == '{' && literal[i + 1] == '}') {
                ++placeholders;
                i += 2;
                continue;
              }
              ++i;
            }
            if (placeholders != sizeof...(Args)) {
              format_placeholder_count_does_not_match_arguments();
            }
          }
          [
            [nodiscard]
          ] constexpr std::string_view view() const noexcept {
            return text_;
          }
        private: std::string_view text_;
      };
    inline void append(std::string & out, std::string_view value) {
      out.append(value);
    }
    inline void append(std::string & out,
      const std::string & value) {
      out.append(value);
    }
    inline void append(std::string & out, char value) {
      out.push_back(value);
    }
    inline void append(std::string & out,
      const char * value) {
      if (value != nullptr) {
        out.append(value);
      }
    }
    template < std::integral T >
      void append(std::string & out, T value) {
        std::array < char, 24 > digits {};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        out.append(digits.data(), result.ptr);
      }
    template < typename...Args > [
      [nodiscard]
    ] std::string format(Spec < std::type_identity_t < Args > ... > spec,
      const Args & ...args) {
      const std::string_view text = spec.view();
      std::string out;
      out.reserve(text.size() + sizeof...(Args) * 8);
      std::size_t cursor = 0;
      const auto copy_to_placeholder = [text, & out, & cursor] {
        while (cursor < text.size()) {
          const char current = text[cursor];
          const bool has_next = cursor + 1 < text.size();
          if ((current == '{' || current == '}') && has_next && text[cursor + 1] == current) {
            out.push_back(current);
            cursor += 2;
            continue;
          }
          if (current == '{' && has_next && text[cursor + 1] == '}') {
            cursor += 2;
            return true;
          }
          out.push_back(current);
          ++cursor;
        }
        return false;
      };
      (void)((copy_to_placeholder() ? (append(out, args), true) : false) && ...);
      (void) copy_to_placeholder();
      return out;
    }
  }
  namespace status {
    sys::Mutex lock;
    std::string line = "Initializing...";
    bool feed_done = false;
    template < typename...Args >
      void print(fmt::Spec < std::type_identity_t < Args > ... > spec,
        const Args & ...args) {
        std::string text = fmt::format(spec, args...);
        std::scoped_lock guard {
          lock
        };
        line = std::move(text);
      }
      [
        [nodiscard]
      ] std::string current() {
        std::scoped_lock guard {
          lock
        };
        return line;
      }
    void mark_feed_done() {
        std::scoped_lock guard {
          lock
        };
        feed_done = true;
      }
      [
        [nodiscard]
      ] bool feed_finished() {
        std::scoped_lock guard {
          lock
        };
        return feed_done;
      }
  }
  namespace sd {
    sys::RecursiveMutex path_lock;
    using PathGuard = std::scoped_lock < sys::RecursiveMutex > ;
    class DirHandle {
      public: explicit DirHandle(const char * path) noexcept: dir_ {
        ::opendir(path)
      } {}
      DirHandle(const DirHandle & ) = delete;
      DirHandle & operator = (const DirHandle & ) = delete;
      ~DirHandle() {
        if (dir_ != nullptr) {
          ::closedir(dir_);
        }
      }
      [
        [nodiscard]
      ] DIR * get() const noexcept {
          return dir_;
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return dir_ != nullptr;
        }
      private: DIR * dir_ = nullptr;
    };
    [
      [nodiscard]
    ] std::string join(std::string_view directory, std::string_view name) {
        std::string out;
        out.reserve(directory.size() + name.size() + 1);
        out.assign(directory);
        if (!out.empty() && out.back() != '/') {
          out.push_back('/');
        }
        out.append(name);
        return out;
      }
      [
        [nodiscard]
      ] bool remove_tree(std::string_view root) {
        const PathGuard guard {
          path_lock
        };
        std::vector < std::string > pending;
        pending.emplace_back(root);
        while (!pending.empty()) {
          std::string current = std::move(pending.back());
          pending.pop_back();
          const DirHandle dir {
            current.c_str()
          };
          if (!dir) {
            if (errno == ENOENT) {
              continue;
            }
            return false;
          }
          bool descended = false;
          while (const dirent * entry = ::readdir(dir.get())) {
            const std::string_view name {
              entry -> d_name
            };
            if (name == "." || name == "..") {
              continue;
            }
            const std::string full = join(current, name);
            struct stat info {};
            if (::lstat(full.c_str(), & info) != 0) {
              if (errno == ENOENT) {
                continue;
              }
              return false;
            }
            if (S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode)) {
              pending.push_back(std::move(current));
              pending.push_back(full);
              descended = true;
              break;
            }
            if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
              if (::unlink(full.c_str()) != 0) {
                return false;
              }
            } else {
              ::unlink(full.c_str());
            }
          }
          if (!descended && ::rmdir(current.c_str()) != 0 && errno != ENOENT) {
            return false;
          }
        }
        return true;
      }
      [
        [nodiscard]
      ] bool make_directories(std::string_view path) {
        const PathGuard guard {
          path_lock
        };
        std::string full {
          path
        };
        constexpr std::string_view DEVICE = "sdmc:/";
        const std::size_t start = full.starts_with(DEVICE) ? DEVICE.size() : 0;
        for (std::size_t i = start; i < full.size(); ++i) {
          if (full[i] != '/') {
            continue;
          }
          const std::string component = full.substr(0, i);
          if (::mkdir(component.c_str(), 0777) != 0 && errno != EEXIST) {
            status::print("mkdir failed:{}", component);
            return false;
          }
        }
        if (::mkdir(full.c_str(), 0777) != 0 && errno != EEXIST) {
          status::print("mkdir failed:{}", full);
          return false;
        }
        return true;
      }
    void unlink_quietly(const char * path) noexcept {
        const PathGuard guard {
          path_lock
        };::unlink(path);
      }
      [
        [nodiscard]
      ] bool replace_file(const char * source,
        const char * destination) noexcept {
        const PathGuard guard {
          path_lock
        };::unlink(destination);
        if (::rename(source, destination) == 0) {
          return true;
        }::unlink(source);
        return false;
      }
      [
        [nodiscard]
      ] std::optional < int64_t > file_size(const char * path) noexcept {
        const PathGuard guard {
          path_lock
        };
        struct stat info {};
        if (::stat(path, & info) != 0 || info.st_size <= 0) {
          return std::nullopt;
        }
        return static_cast < int64_t > (info.st_size);
      }
      [
        [nodiscard]
      ] bool exists(const char * path) noexcept {
        const PathGuard guard {
          path_lock
        };
        struct stat info {};
        return::stat(path, & info) == 0;
      }
      [
        [nodiscard]
      ] sys::FileHandle open(const char * path,
        const char * mode) noexcept {
        const PathGuard guard {
          path_lock
        };
        return sys::FileHandle {
          std::fopen(path, mode)
        };
      }
      [
        [nodiscard]
      ] bool init_paths() {
        return make_directories(cfg::APP_DIR.view()) &&
          make_directories(cfg::LISTS_DIR.view()) &&
          make_directories(cfg::THUMB_DIR.view()) &&
          make_directories(cfg::CTGP7_DIR.view());
      }
  }
  namespace progress {
    std::atomic < int > jobs_done {
      0
    };
    int jobs_total = 0;
    const char * phase_label = "";
    void begin(const char * label, std::size_t total) {
      phase_label = label;
      jobs_total = static_cast < int > (total);
      jobs_done.store(0, std::memory_order_relaxed);
      status::print("{}0/{}...", label, jobs_total);
    }
    void step() {
      const int done = jobs_done.fetch_add(1, std::memory_order_relaxed) + 1;
      status::print("{}{}/{}...", phase_label, done, jobs_total);
    }
  }
  namespace net {
    struct Response {
      std::string data;
      long status_code = 0;
      bool ok = false;
    };
    std::atomic < int > failed_requests {
      0
    };
    std::array < sys::Mutex, CURL_LOCK_DATA_LAST > share_locks;
    sys::CurlShare share;
    void share_lock_cb(CURL * , curl_lock_data data, curl_lock_access, void * ) {
      if (data < CURL_LOCK_DATA_LAST) {
        share_locks[static_cast < std::size_t > (data)].lock();
      }
    }
    void share_unlock_cb(CURL * , curl_lock_data data, void * ) {
      if (data < CURL_LOCK_DATA_LAST) {
        share_locks[static_cast < std::size_t > (data)].unlock();
      }
    }
    void share_init() {
      (void) share.init( & share_lock_cb, & share_unlock_cb);
    }
    void share_cleanup() {
      share.reset();
    }
    std::size_t write_to_string(void * contents, std::size_t size, std::size_t nmemb, void * userp) {
      const std::size_t total = size * nmemb;
      auto * out = static_cast < std::string * > (userp);
      if (total > 0 && out -> size() > cfg::MAX_RESPONSE_SIZE - total) {
        return CURL_WRITEFUNC_ERROR;
      }
      if (total == 0) {
        return 0;
      }
      out -> append(static_cast <
        const char * > (contents), total);
      return total;
    }
    void configure(CURL * curl) noexcept {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, & write_to_string);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, cfg::USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, cfg::CA_BUNDLE_PATH);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (share.get() != nullptr) {
          curl_easy_setopt(curl, CURLOPT_SHARE, share.get());
        }
      }
      [
        [nodiscard]
      ] Response get(CURL * curl,
        const std::string & url) {
        Response response;
        if (curl == nullptr) {
          return response;
        }
        response.data.reserve(cfg::RESPONSE_RESERVE);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, & response.data);
        if (curl_easy_perform(curl) == CURLE_OK &&
          curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, & response.status_code) == CURLE_OK) {
          response.ok = response.status_code >= 200 && response.status_code < 300;
        }
        if (!response.ok) {
          failed_requests.fetch_add(1, std::memory_order_relaxed);
          response.data.clear();
        }
        return response;
      }
      [
        [nodiscard]
      ] constexpr bool request_too_large(long code) noexcept {
        return code == 400 || code == 413 || code == 414;
      }
      [
        [nodiscard]
      ] Response get_with_retry(CURL * curl,
        const std::string & url, int max_attempts) {
        Response response;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
          if (attempt > 0) {
            svcSleepThread(cfg::RETRY_BASE_DELAY_NS << (attempt - 1));
          }
          response = get(curl, url);
          if (response.ok) {
            return response;
          }
          if (request_too_large(response.status_code)) {
            break;
          }
          if (response.status_code == 429 || response.status_code == 503) {
            svcSleepThread(cfg::THROTTLE_DELAY_NS);
          }
        }
        return response;
      }
    template < typename Fn >
      concept JobFunction = std::invocable < Fn & , CURL * , std::size_t > ;
    template < JobFunction Fn >
      struct JobPool {
        std::atomic < std::size_t > next {
          0
        };
        std::size_t count = 0;
        Fn * job = nullptr;
        void run() noexcept {
          sys::CurlHandle curl;
          if (!curl) {
            return;
          }
          configure(curl.get());
          for (;;) {
            const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
              break;
            }
            ( * job)(curl.get(), index);
          }
        }
        static void entry(void * self) noexcept {
          static_cast < JobPool * > (self) -> run();
        }
      };
    template < JobFunction Fn >
      void run_jobs(std::size_t count, Fn & job) {
        if (count == 0) {
          return;
        }
        JobPool < Fn > pool;
        pool.count = count;
        pool.job = & job;
        {
          sys::CurlHandle warm;
          if (warm) {
            configure(warm.get());
            const std::size_t index = pool.next.fetch_add(1, std::memory_order_relaxed);
            if (index < pool.count) {
              job(warm.get(), index);
            }
          }
        }
        if (pool.next.load(std::memory_order_relaxed) >= pool.count) {
          return;
        }
        const std::size_t remaining = pool.count - pool.next.load(std::memory_order_relaxed);
        const std::size_t wanted = std::min(remaining, cfg::FETCH_WORKERS);
        std::array < sys::Thread, cfg::FETCH_WORKERS > workers;
        std::size_t started = 0;
        for (std::size_t i = 0; i < wanted; ++i) {
          workers[i] = sys::Thread::spawn( & JobPool < Fn > ::entry, & pool,
            cfg::WORKER_STACK_SIZE, cfg::WORKER_PRIORITY,
            cfg::ANY_CORE);
          if (!workers[i]) {
            break;
          }
          ++started;
        }
        if (started == 0) {
          pool.run();
          return;
        }
        for (std::size_t i = 0; i < started; ++i) {
          workers[i].join();
        }
      }
  }
  namespace js {
    [
      [nodiscard]
    ] bool is(json_object * object, json_type type) noexcept {
        return object != nullptr && json_object_get_type(object) == type;
      }
      [
        [nodiscard]
      ] json_object * field(json_object * object,
        const char * key, json_type type) noexcept {
        json_object * value = nullptr;
        if (!json_object_object_get_ex(object, key, & value)) {
          return nullptr;
        }
        return is(value, type) ? value : nullptr;
      }
    template < std::integral T > [
        [nodiscard]
      ] std::optional < T > parse_integer(std::string_view text) noexcept {
        if (text.empty()) {
          return std::nullopt;
        }
        T value {};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc {} || result.ptr != text.data() + text.size()) {
          return std::nullopt;
        }
        return value;
      }
      [
        [nodiscard]
      ]
    const char * string_field(json_object * object,
        const char * key) noexcept {
        json_object * value = field(object, key, json_type_string);
        return value != nullptr ? json_object_get_string(value) : nullptr;
      }
      [
        [nodiscard]
      ] std::string string_or_empty(json_object * object,
        const char * key) {
        const char * text = string_field(object, key);
        return text != nullptr ? std::string {
          text
        } : std::string {};
      }
    template < std::integral T > [
      [nodiscard]
    ] T integer_field(json_object * object,
      const char * key) noexcept {
      json_object * value = nullptr;
      if (!json_object_object_get_ex(object, key, & value) || value == nullptr) {
        return T {};
      }
      switch (json_object_get_type(value)) {
      case json_type_int: {
        const int64_t raw = json_object_get_int64(value);
        if (raw < static_cast < int64_t > (std::numeric_limits < T > ::min()) ||
          raw > static_cast < int64_t > (std::numeric_limits < T > ::max())) {
          return T {};
        }
        return static_cast < T > (raw);
      }
      case json_type_string: {
        const char * text = json_object_get_string(value);
        if (text == nullptr) {
          return T {};
        }
        return parse_integer < T > (std::string_view {
          text
        }).value_or(T {});
      }
      default:
        return T {};
      }
    }
    class ArrayView {
      public: class Iterator {
        public: using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = json_object * ;
        Iterator() noexcept =
        default;
        Iterator(json_object * array, std::size_t index) noexcept: array_ {
          array
        },
        index_ {
          index
        } {}
        [
          [nodiscard]
        ] json_object * operator * () const noexcept {
          return json_object_array_get_idx(array_, index_);
        }
        Iterator & operator++() noexcept {
          ++index_;
          return * this;
        }
        Iterator operator++(int) noexcept {
          Iterator previous = * this;
          ++index_;
          return previous;
        }
        [
          [nodiscard]
        ] bool operator == (const Iterator & ) const noexcept =
          default;
        private: json_object * array_ = nullptr;
        std::size_t index_ = 0;
      };
      explicit ArrayView(json_object * array) noexcept: array_ {
        is(array, json_type_array) ? array : nullptr
      },
      size_ {
        array_ != nullptr ? json_object_array_length(array_) : 0
      } {}
      [
        [nodiscard]
      ] Iterator begin() const noexcept {
          return Iterator {
            array_,
            0
          };
        }
        [
          [nodiscard]
        ] Iterator end() const noexcept {
            return Iterator {
              array_,
              size_
            };
          }
          [
            [nodiscard]
          ] std::size_t size() const noexcept {
              return size_;
            }
            [
              [nodiscard]
            ] bool empty() const noexcept {
              return size_ == 0;
            }
      private: json_object * array_ = nullptr;
      std::size_t size_ = 0;
    };
    [
      [nodiscard]
    ] ArrayView array_field(json_object * object,
        const char * key) noexcept {
        return ArrayView {
          field(object, key, json_type_array)
        };
      }
      [
        [nodiscard]
      ] bool add_field(json_object * object,
        const char * key, json_object * value) noexcept {
        if (value == nullptr) {
          return false;
        }
        if (json_object_object_add(object, key, value) != 0) {
          json_object_put(value);
          return false;
        }
        return true;
      }
      [
        [nodiscard]
      ] sys::JsonRef read_file(const char * path) {
        const sd::PathGuard guard {
          sd::path_lock
        };
        return sys::JsonRef {
          json_object_from_file(path)
        };
      }
      [
        [nodiscard]
      ] bool write_file(const char * path, json_object * root) {
        const sd::PathGuard guard {
          sd::path_lock
        };
        return json_object_to_file_ext(path, root, JSON_C_TO_STRING_NOSLASHESCAPE) >= 0;
      }
  }
  namespace store {
    struct ModData {
      int id = 0;
      std::string name;
      std::string author;
      std::string thumbnail_url;
      std::string latest_file_url;
      int64_t latest_file_date = 0;
      std::string latest_file_name;
    };
    struct InstallRecord {
      int64_t date = 0;
      std::vector < std::string > files;
      std::string source_file_name;
    };
    class Registry {
      public: struct Entry {
        int id = 0;
        InstallRecord record;
      };
      [
        [nodiscard]
      ]
      const InstallRecord * find(int id) const noexcept {
          const auto it = lower_bound(id);
          if (it == entries_.end() || it -> id != id) {
            return nullptr;
          }
          return & it -> record;
        }
        [
          [nodiscard]
        ] bool contains(int id) const noexcept {
          return find(id) != nullptr;
        }
      void insert_or_assign(int id, InstallRecord record) {
        const auto it = lower_bound(id);
        if (it != entries_.end() && it -> id == id) {
          it -> record = std::move(record);
          return;
        }
        entries_.insert(it, Entry {
          id,
          std::move(record)
        });
      }
      bool erase(int id) {
        const auto it = lower_bound(id);
        if (it == entries_.end() || it -> id != id) {
          return false;
        }
        entries_.erase(it);
        return true;
      }
      void clear() noexcept {
        entries_.clear();
      }
      [
        [nodiscard]
      ] std::size_t size() const noexcept {
          return entries_.size();
        }
        [
          [nodiscard]
        ]
      const std::vector < Entry > & entries() const noexcept {
        return entries_;
      }
      private: [
          [nodiscard]
        ] std::vector < Entry > ::iterator lower_bound(int id) noexcept {
          return std::lower_bound(entries_.begin(), entries_.end(), id,
            [](const Entry & entry, int key) {
              return entry.id < key;
            });
        }
        [
          [nodiscard]
        ] std::vector < Entry > ::const_iterator lower_bound(int id) const noexcept {
          return std::lower_bound(entries_.begin(), entries_.end(), id,
            [](const Entry & entry, int key) {
              return entry.id < key;
            });
        }
      std::vector < Entry > entries_;
    };
    Registry installed;
    [
      [nodiscard]
    ] bool write_mod_list(const char * path,
        const std::vector < ModData > & mods) {
        sys::JsonRef array {
          json_object_new_array()
        };
        if (!array) {
          return false;
        }
        for (const ModData & mod: mods) {
          sys::JsonRef entry {
            json_object_new_object()
          };
          if (!entry) {
            return false;
          }
          const bool built =
            js::add_field(entry.get(), "Id", json_object_new_int(mod.id)) &&
            js::add_field(entry.get(), "Name", json_object_new_string(mod.name.c_str())) &&
            js::add_field(entry.get(), "Author", json_object_new_string(mod.author.c_str())) &&
            js::add_field(entry.get(), "ThumbnailUrl",
              json_object_new_string(mod.thumbnail_url.c_str())) &&
            js::add_field(entry.get(), "LatestFileUrl",
              json_object_new_string(mod.latest_file_url.c_str())) &&
            js::add_field(entry.get(), "LatestFileDate",
              json_object_new_int64(mod.latest_file_date)) &&
            js::add_field(entry.get(), "LatestFileName",
              json_object_new_string(mod.latest_file_name.c_str()));
          if (!built) {
            return false;
          }
          if (json_object_array_add(array.get(), entry.get()) != 0) {
            return false;
          }
          (void) entry.release();
        }
        return js::write_file(path, array.get());
      }
      [
        [nodiscard]
      ] bool read_mod_list(const char * path, std::vector < ModData > & out) {
        out.clear();
        const sys::JsonRef root = js::read_file(path);
        if (!root || !js::is(root.get(), json_type_array)) {
          return false;
        }
        const js::ArrayView records {
          root.get()
        };
        out.reserve(records.size());
        for (json_object * record: records) {
          if (!js::is(record, json_type_object)) {
            continue;
          }
          ModData mod;
          mod.id = js::integer_field < int > (record, "Id");
          if (mod.id == 0) {
            continue;
          }
          mod.name = js::string_or_empty(record, "Name");
          mod.author = js::string_or_empty(record, "Author");
          mod.thumbnail_url = js::string_or_empty(record, "ThumbnailUrl");
          mod.latest_file_url = js::string_or_empty(record, "LatestFileUrl");
          mod.latest_file_name = js::string_or_empty(record, "LatestFileName");
          mod.latest_file_date = js::integer_field < int64_t > (record, "LatestFileDate");
          out.push_back(std::move(mod));
        }
        return true;
      }
      [
        [nodiscard]
      ] bool load_installed() {
        installed.clear();
        sys::JsonRef root = js::read_file(cfg::INSTALLED_FILE.c_str());
        if (!root) {
          root = js::read_file(cfg::INSTALLED_TMP.c_str());
        }
        if (!root) {
          return true;
        }
        if (!js::is(root.get(), json_type_object)) {
          return false;
        }
        json_object_object_foreach(root.get(), key, value) {
          if (key == nullptr || !js::is(value, json_type_object)) {
            continue;
          }
          const auto id = js::parse_integer < int > (std::string_view {
            key
          });
          if (!id || * id <= 0) {
            continue;
          }
          InstallRecord record;
          record.date = js::integer_field < int64_t > (value, "Date");
          record.source_file_name = js::string_or_empty(value, "SourceFileName");
          for (json_object * file: js::array_field(value, "Files")) {
            if (!js::is(file, json_type_string)) {
              continue;
            }
            const char * name = json_object_get_string(file);
            if (name != nullptr && name[0] != '\0') {
              record.files.emplace_back(name);
            }
          }
          installed.insert_or_assign( * id, std::move(record));
        }
        sd::unlink_quietly(cfg::INSTALLED_TMP.c_str());
        return true;
      }
      [
        [nodiscard]
      ] bool save_installed() {
        sys::JsonRef root {
          json_object_new_object()
        };
        if (!root) {
          return false;
        }
        for (const Registry::Entry & entry: installed.entries()) {
          sys::JsonRef record {
            json_object_new_object()
          };
          if (!record) {
            return false;
          }
          json_object * files = json_object_new_array();
          if (!js::add_field(record.get(), "Files", files)) {
            return false;
          }
          for (const std::string & file: entry.record.files) {
            json_object * item = json_object_new_string(file.c_str());
            if (item == nullptr) {
              return false;
            }
            if (json_object_array_add(files, item) != 0) {
              json_object_put(item);
              return false;
            }
          }
          if (!js::add_field(record.get(), "Date",
              json_object_new_int64(entry.record.date)) ||
            !js::add_field(record.get(), "SourceFileName",
              json_object_new_string(entry.record.source_file_name.c_str()))) {
            return false;
          }
          const std::string key = fmt::format("{}", entry.id);
          if (!js::add_field(root.get(), key.c_str(), record.get())) {
            return false;
          }
          (void) record.release();
        }
        if (!js::write_file(cfg::INSTALLED_TMP.c_str(), root.get())) {
          sd::unlink_quietly(cfg::INSTALLED_TMP.c_str());
          return false;
        }
        return sd::replace_file(cfg::INSTALLED_TMP.c_str(), cfg::INSTALLED_FILE.c_str());
      }
  }
  namespace feed {
    using store::ModData;
    void parse_index_records(json_object * records, std::vector < ModData > & out) {
      for (json_object * record: js::ArrayView {
          records
        }) {
        if (!js::is(record, json_type_object)) {
          continue;
        }
        const int id = js::integer_field < int > (record, "_idRow");
        if (id == 0) {
          continue;
        }
        ModData mod;
        mod.id = id;
        mod.name = js::string_or_empty(record, "_sName");
        mod.author = "Unknown";
        if (json_object * submitter = js::field(record, "_aSubmitter", json_type_object)) {
          const char * name = js::string_field(submitter, "_sName");
          if (name != nullptr && name[0] != '\0') {
            mod.author = name;
          }
        }
        if (json_object * preview = js::field(record, "_aPreviewMedia", json_type_object)) {
          const js::ArrayView images = js::array_field(preview, "_aImages");
          if (!images.empty()) {
            json_object * image = * images.begin();
            if (js::is(image, json_type_object)) {
              const char * base = js::string_field(image, "_sBaseUrl");
              const char * file220 = js::string_field(image, "_sFile220");
              const char * file = js::string_field(image, "_sFile");
              const char * chosen = nullptr;
              if (file220 != nullptr && file220[0] != '\0') {
                chosen = file220;
              } else if (file != nullptr && file[0] != '\0') {
                chosen = file;
              }
              if (base != nullptr && base[0] != '\0' && chosen != nullptr) {
                mod.thumbnail_url = fmt::format("{}/{}", base, chosen);
              }
            }
          }
        }
        out.push_back(std::move(mod));
      }
    }
    struct PageResult {
      std::vector < ModData > mods;
      int record_count = -1;
      bool ok = false;
    };
    [
      [nodiscard]
    ] bool fetch_index_page(CURL * curl, int category_id, int page, PageResult & out) {
      const std::string url = fmt::format(
        "{}?_nPage={}&_nPerpage={}&_aFilters[Generic_Category]={}",
        cfg::API_V10_INDEX, page, cfg::INDEX_PER_PAGE, category_id);
      const net::Response response = net::get_with_retry(curl, url, cfg::MAX_FETCH_ATTEMPTS);
      if (!response.ok) {
        return false;
      }
      const sys::JsonRef root {
        json_tokener_parse(response.data.c_str())
      };
      if (!root) {
        return false;
      }
      if (json_object * meta = js::field(root.get(), "_aMetadata", json_type_object)) {
        out.record_count = js::integer_field < int > (meta, "_nRecordCount");
      }
      parse_index_records(js::field(root.get(), "_aRecords", json_type_array), out.mods);
      out.ok = true;
      return true;
    }
    struct PageJob {
      int category_id = 0;
      int page = 1;
      bool sequential = false;
    };
    void collect_pages(const std::vector < PageJob > & jobs, std::vector < PageResult > & results) {
      results.clear();
      results.resize(jobs.size());
      if (jobs.empty()) {
        return;
      }
      auto run_page_job = [ & jobs, & results](CURL * curl, std::size_t index) {
        const PageJob & job = jobs[index];
        PageResult & out = results[index];
        if (!job.sequential) {
          (void) fetch_index_page(curl, job.category_id, job.page, out);
        } else {
          for (int page = job.page; page < job.page + cfg::MAX_PAGES_PER_CAT; ++page) {
            const std::size_t before = out.mods.size();
            if (!fetch_index_page(curl, job.category_id, page, out)) {
              break;
            }
            if (out.mods.size() - before < static_cast < std::size_t > (cfg::INDEX_PER_PAGE)) {
              break;
            }
          }
        }
        progress::step();
      };
      net::run_jobs(jobs.size(), run_page_job);
    }
    void deduplicate(std::vector < ModData > & mods) {
      if (mods.size() <= 1) {
        return;
      }
      std::ranges::sort(mods, {}, & ModData::id);
      const auto duplicates = std::ranges::unique(mods, {}, & ModData::id);
      mods.erase(duplicates.begin(), duplicates.end());
    }
    void parse_latest_file(json_object * raw_item, ModData & mod) {
      json_object * item = raw_item;
      if (js::is(raw_item, json_type_array)) {
        const js::ArrayView wrapper {
          raw_item
        };
        if (wrapper.empty()) {
          return;
        }
        item = * wrapper.begin();
      }
      if (!js::is(item, json_type_object)) {
        return;
      }
      int64_t newest = 0;
      const auto consider = [ & mod, & newest](json_object * file) {
        if (!js::is(file, json_type_object)) {
          return;
        }
        const int file_id = js::integer_field < int > (file, "_idRow");
        const int64_t added = js::integer_field < int64_t > (file, "_tsDateAdded");
        const char *
          const name = js::string_field(file, "_sFile");
        if (file_id > 0 && name != nullptr && name[0] != '\0' && added > newest) {
          newest = added;
          mod.latest_file_url = fmt::format("{}{}", cfg::DOWNLOAD_BASE, file_id);
          mod.latest_file_name = name;
          mod.latest_file_date = added;
        }
      };
      const js::ArrayView files = js::array_field(item, "aFiles");
      if (!files.empty()) {
        for (json_object * file: files) {
          consider(file);
        }
        return;
      }
      json_object_object_foreach(item, key, value) {
        (void) key;
        consider(value);
      }
    }
    std::atomic < std::size_t > core_batch_cap {
      cfg::CORE_BATCH_SIZE
    };
    void lower_core_cap(std::size_t limit) noexcept {
      const std::size_t target = std::max < std::size_t > (limit, 1);
      std::size_t current = core_batch_cap.load(std::memory_order_relaxed);
      while (target < current &&
        !core_batch_cap.compare_exchange_weak(current, target, std::memory_order_relaxed)) {}
    }
    void fetch_core_range(CURL * curl, std::span < ModData > mods) {
      if (mods.empty()) {
        return;
      }
      const auto split = [curl](std::span < ModData > range) {
        const std::size_t half = range.size() / 2;
        fetch_core_range(curl, range.first(half));
        fetch_core_range(curl, range.subspan(half));
      };
      if (mods.size() > 1 && mods.size() > core_batch_cap.load(std::memory_order_relaxed)) {
        split(mods);
        return;
      }
      std::vector < ModData * > targets;
      targets.reserve(mods.size());
      std::string url {
        cfg::API_CORE_DATA
      };
      url += '?';
      for (ModData & mod: mods) {
        if (!mod.latest_file_url.empty()) {
          continue;
        }
        url += fmt::format("itemtype[]=Mod&itemid[]={}&fields[]=Files().aFiles()&", mod.id);
        targets.push_back( & mod);
      }
      if (targets.empty()) {
        return;
      }
      url.pop_back();
      bool resolved = false;
      bool too_big = false;
      const net::Response response = net::get_with_retry(curl, url, cfg::MAX_FETCH_ATTEMPTS);
      if (response.ok) {
        const sys::JsonRef root {
          json_tokener_parse(response.data.c_str())
        };
        if (root) {
          if (targets.size() == 1) {
            parse_latest_file(root.get(), * targets[0]);
            resolved = true;
          } else {
            const js::ArrayView items {
              root.get()
            };
            if (items.size() == targets.size()) {
              std::size_t index = 0;
              for (json_object * item: items) {
                if (item != nullptr) {
                  parse_latest_file(item, * targets[index]);
                }
                ++index;
              }
              resolved = true;
            }
          }
        }
        too_big = !resolved;
      } else if (net::request_too_large(response.status_code)) {
        too_big = true;
      }
      if (resolved || mods.size() == 1) {
        return;
      }
      if (too_big) {
        lower_core_cap(mods.size() / 2);
      }
      split(mods);
    }
    void fetch_core_data(std::vector < ModData > & mods) {
      if (mods.empty()) {
        return;
      }
      const std::size_t total = mods.size();
      const std::size_t batches = (total + cfg::CORE_BATCH_SIZE - 1) / cfg::CORE_BATCH_SIZE;
      auto run_batch = [ & mods, total](CURL * curl, std::size_t index) {
        const std::size_t first = index * cfg::CORE_BATCH_SIZE;
        const std::size_t count = std::min(cfg::CORE_BATCH_SIZE, total - first);
        fetch_core_range(curl, std::span {
          mods
        }.subspan(first, count));
        progress::step();
      };
      progress::begin("Resolving downloads ", batches);
      net::run_jobs(batches, run_batch);
    }
    void drain_pages(std::vector < PageResult > & results, std::vector < ModData > & out) {
        std::size_t incoming = 0;
        for (const PageResult & result: results) {
          incoming += result.mods.size();
        }
        out.reserve(out.size() + incoming);
        for (PageResult & result: results) {
          out.insert(out.end(),
            std::make_move_iterator(result.mods.begin()),
            std::make_move_iterator(result.mods.end()));
          std::vector < ModData > {}.swap(result.mods);
        }
      }
      [
        [nodiscard]
      ] std::vector < PageJob > plan_remaining_pages(const std::vector < PageResult > & first_pass) {
        std::vector < PageJob > jobs;
        for (std::size_t i = 0; i < cfg::CATEGORIES.size(); ++i) {
          const PageResult & result = first_pass[i];
          if (!result.ok ||
            result.mods.size() < static_cast < std::size_t > (cfg::INDEX_PER_PAGE)) {
            continue;
          }
          PageJob job;
          job.category_id = cfg::CATEGORIES[i];
          if (result.record_count > cfg::INDEX_PER_PAGE) {
            int pages = (result.record_count + cfg::INDEX_PER_PAGE - 1) / cfg::INDEX_PER_PAGE;
            pages = std::min(pages, cfg::MAX_PAGES_PER_CAT);
            for (int page = 2; page < pages; ++page) {
              job.page = page;
              job.sequential = false;
              jobs.push_back(job);
            }
            job.page = pages;
          } else {
            job.page = 2;
          }
          job.sequential = true;
          jobs.push_back(job);
        }
        return jobs;
      }
    void run_pipeline() {
      std::vector < ModData > all_mods;
      std::vector < PageJob > first_jobs(cfg::CATEGORIES.size());
      for (std::size_t i = 0; i < cfg::CATEGORIES.size(); ++i) {
        first_jobs[i].category_id = cfg::CATEGORIES[i];
        first_jobs[i].page = 1;
      }
      std::vector < PageResult > first_results;
      progress::begin("Scanning categories ", first_jobs.size());
      collect_pages(first_jobs, first_results);
      const std::vector < PageJob > more_jobs = plan_remaining_pages(first_results);
      drain_pages(first_results, all_mods);
      if (!more_jobs.empty()) {
        std::vector < PageResult > more_results;
        progress::begin("Fetching mod pages ", more_jobs.size());
        collect_pages(more_jobs, more_results);
        drain_pages(more_results, all_mods);
      }
      if (all_mods.empty()) {
        status::print("Failed to fetch any mods!");
        return;
      }
      deduplicate(all_mods);
      fetch_core_data(all_mods);
      status::print("Saving...");
      const std::size_t resolved = static_cast < std::size_t > (
        std::ranges::count_if(all_mods, [](const ModData & mod) {
          return !mod.latest_file_url.empty();
        }));
      if (!store::write_mod_list(cfg::MOD_LIST_FILE.c_str(), all_mods)) {
        status::print("Error saving mod list!");
      }
      std::ranges::sort(all_mods, {}, & ModData::name);
      if (!store::write_mod_list(cfg::BY_NAME_FILE.c_str(), all_mods)) {
        status::print("Warning: failed to write byname.json");
      }
      std::ranges::sort(all_mods, std::ranges::greater {}, & ModData::latest_file_date);
      if (!store::write_mod_list(cfg::BY_UPDATED_FILE.c_str(), all_mods)) {
        status::print("Warning: failed to write byupdated.json");
      }
      const int failures = net::failed_requests.load(std::memory_order_relaxed);
      if (failures > 0) {
        status::print("Done: {} mods, {} resolved, {} request(s) retried/failed.",
          all_mods.size(), resolved, failures);
      } else {
        status::print("Done! Enriched {}/{} mods.", resolved, all_mods.size());
      }
    }
    void thread_main(void * ) {
      (void) sd::remove_tree(cfg::LISTS_DIR.view());
      (void) sd::init_paths();
      net::share_init();
      run_pipeline();
      net::share_cleanup();
      status::mark_feed_done();
    }
  }
  namespace audio {
    OggVorbis_File stream {};
    bool loop_mode = false;
    std::atomic < bool > should_stop {
      false
    };
    long sample_rate = 32768;
    int channel_count = 1;
    bool ndsp_ready = false;
    std::array < ndspWaveBuf, 2 > wave_buffers {};
    std::array < sys::LinearBuffer, 2 > wave_memory;
    sys::Thread worker;
    std::size_t ogg_read(void * dest, std::size_t size, std::size_t count, void * source) {
      return std::fread(dest, size, count, static_cast < std::FILE * > (source));
    }
    int ogg_seek(void * source, ogg_int64_t offset, int whence) {
      if (offset < static_cast < ogg_int64_t > (std::numeric_limits < long > ::min()) ||
        offset > static_cast < ogg_int64_t > (std::numeric_limits < long > ::max())) {
        return -1;
      }
      return std::fseek(static_cast < std::FILE * > (source), static_cast < long > (offset), whence) != 0 ?
        -1 :
        0;
    }
    int ogg_close(void * source) {
      return std::fclose(static_cast < std::FILE * > (source));
    }
    long ogg_tell(void * source) {
      return std::ftell(static_cast < std::FILE * > (source));
    }
    const ov_callbacks CALLBACKS {
      & ogg_read, & ogg_seek, & ogg_close, & ogg_tell
    };
    [
      [nodiscard]
    ] bool load(const char * path) {
      ov_clear( & stream);
      sys::FileHandle file = sd::open(path, "rb");
      if (!file) {
        return false;
      }
      if (ov_open_callbacks(file.get(), & stream, nullptr, 0, CALLBACKS) < 0) {
        return false;
      }
      (void) file.release();
      return true;
    }
    void play(bool loop) {
        loop_mode = loop;
        vorbis_info * info = ov_info( & stream, -1);
        if (info == nullptr) {
          return;
        }
        channel_count = std::clamp(info -> channels, 1, cfg::AUDIO_MAX_CHANNELS);
        sample_rate = info -> rate;
        ndspChnReset(0);
        ndspChnSetFormat(0, channel_count == 1 ? NDSP_FORMAT_MONO_PCM16 :
          NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetRate(0, static_cast < float > (sample_rate));
        std::array < float, 12 > mix {};
        mix[0] = mix[1] = static_cast < float > (cfg::AUDIO_VOL) / 255.0f;
        ndspChnSetMix(0, mix.data());
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetMasterVol(1.0f);
      }
      [
        [nodiscard]
      ] int decode_frames(std::span < s16 > destination, bool & reached_end) {
        reached_end = false;
        const int bytes_capacity = static_cast < int > (destination.size_bytes());
        char * out = reinterpret_cast < char * > (destination.data());
        int bytes_done = 0;
        constexpr int MAX_CONSECUTIVE_ERRORS = 64;
        int consecutive_errors = 0;
        while (bytes_done < bytes_capacity) {
          int bitstream = 0;
          const long read = ov_read( & stream, out + bytes_done,
            bytes_capacity - bytes_done, & bitstream);
          if (read == 0) {
            reached_end = true;
            break;
          }
          if (read < 0) {
            if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
              reached_end = true;
              break;
            }
            continue;
          }
          consecutive_errors = 0;
          bytes_done += static_cast < int > (read);
        }
        return bytes_done / (channel_count * static_cast < int > (sizeof(s16)));
      }
    void thread_main(void * ) {
      play(false);
      int current = 0;
      bool first_buffer = true;
      while (!should_stop.load(std::memory_order_relaxed)) {
        ndspWaveBuf & buffer = wave_buffers[static_cast < std::size_t > (current)];
        if (!first_buffer) {
          while (buffer.status != NDSP_WBUF_DONE &&
            !should_stop.load(std::memory_order_relaxed)) {
            svcSleepThread(1'000'000ULL);
          }
          if (should_stop.load(std::memory_order_relaxed)) {
            break;
          }
        }
        first_buffer = false;
        const std::size_t sample_count =
          static_cast < std::size_t > (cfg::AUDIO_BUF_SAMPLES) *
          static_cast < std::size_t > (channel_count);
        const std::span < s16 > pcm {
          buffer.data_pcm16, sample_count
        };
        bool reached_end = false;
        const int frames = decode_frames(pcm, reached_end);
        const std::size_t decoded_samples =
          static_cast < std::size_t > (frames) * static_cast < std::size_t > (channel_count);
        if (decoded_samples < sample_count) {
          const std::span < s16 > tail = pcm.subspan(decoded_samples);
          std::memset(tail.data(), 0, tail.size_bytes());
        }
        buffer.nsamples = cfg::AUDIO_BUF_SAMPLES;
        buffer.status = NDSP_WBUF_DONE;
        ndspChnWaveBufAdd(0, & buffer);
        current ^= 1;
        if (!reached_end) {
          continue;
        }
        if (loop_mode) {
          ov_raw_seek( & stream, 0);
          continue;
        }
        if (!load("romfs:/loop.ogg")) {
          status::print("Audio: loop.ogg missing");
          break;
        }
        play(true);
        first_buffer = true;
      }
    }
    void init() {
      status::print("Audio: starting...");
      if (R_FAILED(ndspInit())) {
        status::print("Audio: ndspInit failed");
        return;
      }
      ndsp_ready = true;
      if (!load("romfs:/intro.ogg")) {
        status::print("Audio: intro.ogg missing");
        return;
      } {
        int bitstream = 0;
        std::array < char, 4096 > probe {};
        const long decoded = ov_read( & stream, probe.data(),
          static_cast < int > (probe.size()), & bitstream);
        if (decoded <= 0) {
          status::print("Audio: intro.ogg decode failed ({})", decoded);
          ov_clear( & stream);
          return;
        }
        ov_raw_seek( & stream, 0);
      }
      for (sys::LinearBuffer & block: wave_memory) {
        block = sys::LinearBuffer {
          cfg::AUDIO_BUF_BYTES
        };
        if (!block) {
          status::print("Audio: linearAlloc failed");
          for (sys::LinearBuffer & allocated: wave_memory) {
            allocated.reset();
          }
          return;
        }
      }
      for (std::size_t i = 0; i < wave_buffers.size(); ++i) {
        std::memset(wave_memory[i].get(), 0, cfg::AUDIO_BUF_BYTES);
        wave_buffers[i].data_pcm16 = wave_memory[i].as < s16 > ();
        wave_buffers[i].nsamples = cfg::AUDIO_BUF_SAMPLES;
        wave_buffers[i].looping = false;
        wave_buffers[i].status = NDSP_WBUF_DONE;
      }
      worker = sys::Thread::spawn( & thread_main, nullptr, 128 * 1024,
        cfg::AUDIO_PRIORITY, cfg::ANY_CORE);
      if (!worker) {
        status::print("Audio: thread creation failed");
        return;
      }
      status::print("Audio: thread started");
    }
    void shutdown() {
      should_stop.store(true, std::memory_order_relaxed);
      worker.join();
      if (ndsp_ready) {
        ndspChnReset(0);
        ndspExit();
        ndsp_ready = false;
      }
      ov_clear( & stream);
      for (sys::LinearBuffer & block: wave_memory) {
        block.reset();
      }
    }
  }
  namespace text {
    inline constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";
    [
      [nodiscard]
    ] constexpr std::size_t sequence_length(unsigned char lead) noexcept {
        if (lead < 0x80) {
          return 1;
        }
        if ((lead & 0xE0) == 0xC0) {
          return 2;
        }
        if ((lead & 0xF0) == 0xE0) {
          return 3;
        }
        if ((lead & 0xF8) == 0xF0) {
          return 4;
        }
        return 1;
      }
      [
        [nodiscard]
      ] constexpr std::size_t step(std::string_view input, std::size_t index) noexcept {
        const std::size_t length = sequence_length(static_cast < unsigned char > (input[index]));
        return length <= input.size() - index ? length : 1;
      }
    void char_starts(std::string_view input, std::vector < std::size_t > & out) {
        out.clear();
        for (std::size_t i = 0; i < input.size(); i += step(input, i)) {
          out.push_back(i);
        }
      }
      [
        [nodiscard]
      ] constexpr std::string_view clamp_bytes(std::string_view input,
        std::size_t max_bytes) noexcept {
        if (input.size() <= max_bytes) {
          return input;
        }
        std::size_t cut = 0;
        while (cut < input.size()) {
          const std::size_t width = step(input, cut);
          if (cut + width > max_bytes) {
            break;
          }
          cut += width;
        }
        return input.substr(0, cut);
      }
    template < typename Fn >
      concept WidthMeasure = std::invocable < Fn & ,
      const char * > &&
        std::convertible_to < std::invoke_result_t < Fn & ,
        const char * > , float > ;
    template < WidthMeasure Fn > [
      [nodiscard]
    ] std::string fit(std::string_view input, float max_width, Fn measure) {
      if (input.empty() || max_width <= 0.0f) {
        return {};
      }
      const std::string source {
        clamp_bytes(input, cfg::MEASURE_MAX_LEN)
      };
      if (measure(source.c_str()) <= max_width) {
        return source;
      }
      std::vector < std::size_t > starts;
      char_starts(source, starts);
      std::size_t low = 0;
      std::size_t high = starts.size();
      std::size_t best = 0;
      while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        std::string candidate = source.substr(0, starts[middle]);
        candidate.append(ELLIPSIS);
        if (measure(candidate.c_str()) <= max_width) {
          best = middle;
          low = middle + 1;
        } else {
          high = middle;
        }
      }
      if (best == 0) {
        return std::string {
          ELLIPSIS
        };
      }
      std::string result = source.substr(0, starts[best]);
      result.append(ELLIPSIS);
      return result;
    }
  }
  namespace model {
    using store::ModData;
    enum class AppState {
      FETCHING,
      LOADING,
      BROWSING,
      FAILED
    };
    enum class ModAction {
      NONE,
      INSTALL,
      UPDATE,
      INSTALLED
    };
    enum class Priority: int {
      NOT_INSTALLED = 1, INSTALLED = 2, UPDATE_AVAILABLE = 3
    };
    AppState state = AppState::FETCHING;
    std::vector < ModData > mods;
    int window_start = 0;
    int selected = 0;
    bool sort_by_name = false;
    std::string error_text;
    bool cards_dirty = true;
    u32 nav_held_key = 0;
    float nav_timer = 0.0f;
    [
      [nodiscard]
    ] Priority priority_of(const ModData & mod) noexcept {
      const store::InstallRecord * record = store::installed.find(mod.id);
      if (record == nullptr) {
        return Priority::NOT_INSTALLED;
      }
      return mod.latest_file_date > record -> date ? Priority::UPDATE_AVAILABLE :
        Priority::INSTALLED;
    }
    void sort() {
        cards_dirty = true;
        const std::size_t count = mods.size();
        if (count <= 1) {
          return;
        }
        struct SortKey {
          Priority priority;
          u32 index;
        };
        std::vector < SortKey > keys(count);
        for (std::size_t i = 0; i < count; ++i) {
          keys[i] = SortKey {
            priority_of(mods[i]), static_cast < u32 > (i)
          };
        }
        const bool by_name = sort_by_name;
        std::stable_sort(keys.begin(), keys.end(), [by_name](const SortKey & a,
          const SortKey & b) {
          if (a.priority != b.priority) {
            return a.priority > b.priority;
          }
          const ModData & left = mods[a.index];
          const ModData & right = mods[b.index];
          return by_name ? left.name < right.name : left.latest_file_date > right.latest_file_date;
        });
        std::vector < ModData > ordered;
        ordered.reserve(count);
        for (const SortKey & key: keys) {
          ordered.push_back(std::move(mods[key.index]));
        }
        mods = std::move(ordered);
      }
      [
        [nodiscard]
      ] int total_count() noexcept {
        return static_cast < int > (mods.size());
      }
      [
        [nodiscard]
      ] int visible_count() noexcept {
        const int remaining = total_count() - window_start;
        if (remaining <= 0) {
          return 0;
        }
        return std::min(remaining, cfg::CARDS_PER_PAGE);
      }
      [
        [nodiscard]
      ] int max_window_start() noexcept {
        const int total = total_count();
        if (total <= cfg::CARDS_PER_PAGE) {
          return 0;
        }
        const int rows = (total + cfg::GRID_COLS - 1) / cfg::GRID_COLS;
        const int start_row = rows - cfg::GRID_ROWS;
        return start_row > 0 ? start_row * cfg::GRID_COLS : 0;
      }
      [
        [nodiscard]
      ]
    const ModData * selected_mod() noexcept {
        const int visible = visible_count();
        if (visible <= 0 || selected < 0 || selected >= visible) {
          return nullptr;
        }
        return & mods[static_cast < std::size_t > (window_start + selected)];
      }
      [
        [nodiscard]
      ] ModAction current_action() noexcept {
        const ModData * mod = selected_mod();
        if (mod == nullptr) {
          return ModAction::NONE;
        }
        const store::InstallRecord * record = store::installed.find(mod -> id);
        if (record == nullptr) {
          return ModAction::INSTALL;
        }
        return mod -> latest_file_date > record -> date ? ModAction::UPDATE : ModAction::INSTALLED;
      }
    void clamp_view() noexcept {
      window_start = std::clamp(window_start, 0, max_window_start());
      const int visible = visible_count();
      selected = visible > 0 ? std::clamp(selected, 0, visible - 1) : 0;
    }
    void resort_after_change() {
      sort();
      clamp_view();
      cards_dirty = true;
    }
    bool scroll(int rows) noexcept {
      const int before = window_start;
      window_start = std::clamp(window_start + rows * cfg::GRID_COLS, 0, max_window_start());
      if (window_start == before) {
        return false;
      }
      const int visible = visible_count();
      selected = visible > 0 ? std::clamp(selected, 0, visible - 1) : 0;
      cards_dirty = true;
      return true;
    }
    void handle_nav(u32 keys) noexcept {
        const int visible = visible_count();
        if (visible <= 0) {
          return;
        }
        int row_delta = 0;
        int col_delta = 0;
        if (keys & (KEY_DRIGHT | KEY_CPAD_RIGHT)) {
          col_delta = 1;
        } else if (keys & (KEY_DLEFT | KEY_CPAD_LEFT)) {
          col_delta = -1;
        } else if (keys & (KEY_DDOWN | KEY_CPAD_DOWN)) {
          row_delta = 1;
        } else if (keys & (KEY_DUP | KEY_CPAD_UP)) {
          row_delta = -1;
        } else {
          return;
        }
        const int row = selected / cfg::GRID_COLS;
        const int col = selected % cfg::GRID_COLS;
        if (col_delta != 0) {
          const int target_col = std::clamp(col + col_delta, 0, cfg::GRID_COLS - 1);
          selected = std::min(row * cfg::GRID_COLS + target_col, visible - 1);
          return;
        }
        const int last_row = (visible - 1) / cfg::GRID_COLS;
        const int target_row = row + row_delta;
        if (target_row >= 0 && target_row <= last_row) {
          selected = std::min(target_row * cfg::GRID_COLS + col, visible - 1);
          return;
        }
        (void) scroll(row_delta);
      }
      [
        [nodiscard]
      ] u32 nav_repeat(u32 pressed, u32 held, float delta_seconds) noexcept {
        constexpr u32 NAV_MASK = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT |
          KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
        if (const u32 fresh = pressed & NAV_MASK) {
          nav_held_key = fresh;
          nav_timer = cfg::NAV_REPEAT_DELAY;
          return fresh;
        }
        if (nav_held_key == 0 || (held & nav_held_key) == 0) {
          nav_held_key = 0;
          return 0;
        }
        nav_timer -= delta_seconds;
        if (nav_timer > 0.0f) {
          return 0;
        }
        nav_timer += cfg::NAV_REPEAT_RATE;
        if (nav_timer < 0.0f) {
          nav_timer = 0.0f;
        }
        return nav_held_key;
      }
    void set_sort_mode(bool by_name) {
      if (sort_by_name == by_name) {
        return;
      }
      sort_by_name = by_name;
      sort();
      window_start = 0;
      selected = 0;
      cards_dirty = true;
    }
  }
  namespace install {
    [
      [nodiscard]
    ] constexpr char ascii_lower(char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast < char > (c - 'A' + 'a') : c;
      }
      [
        [nodiscard]
      ] constexpr bool ends_with_ci(std::string_view text, std::string_view suffix) noexcept {
        if (suffix.size() > text.size()) {
          return false;
        }
        return std::ranges::equal(text.substr(text.size() - suffix.size()), suffix,
          [](char a, char b) {
            return ascii_lower(a) == ascii_lower(b);
          });
      }
      [
        [nodiscard]
      ] constexpr bool equals_ci(std::string_view a, std::string_view b) noexcept {
        return std::ranges::equal(a, b, [](char x, char y) {
          return ascii_lower(x) == ascii_lower(y);
        });
      }
      [
        [nodiscard]
      ] constexpr std::string sanitize_entry_name(std::string_view raw) {
        if (raw.empty()) {
          return {};
        }
        const std::size_t separator = raw.find_last_of("/\\");
        const std::string_view name =
          separator == std::string_view::npos ? raw : raw.substr(separator + 1);
        if (name.empty()) {
          return {};
        }
        if (name == "." || name == "..") {
          return {};
        }
        if (name.size() > cfg::INSTALL_NAME_MAX) {
          return {};
        }
        for (const char character: name) {
          const auto value = static_cast < unsigned char > (character);
          if (value < 0x20 || value == 0x7F) {
            return {};
          }
          if (value == ':' || value == '*' || value == '?' || value == '"' ||
            value == '<' || value == '>' || value == '|') {
            return {};
          }
        }
        return std::string {
          name
        };
      }
      [
        [nodiscard]
      ] consteval bool sanitize_entry_name_is_safe() {
        const auto rejects = [](std::string_view input) {
          return sanitize_entry_name(input).empty();
        };
        const auto yields = [](std::string_view input, std::string_view expected) {
          return sanitize_entry_name(input) == expected;
        };
        if (!yields("mario.chpack", "mario.chpack")) {
          return false;
        }
        if (!yields("sub/dir/mario.chpack", "mario.chpack")) {
          return false;
        }
        if (!yields("..\\..\\boot.firm", "boot.firm")) {
          return false;
        }
        if (!yields("a/b\\c/evil.chpack", "evil.chpack")) {
          return false;
        }
        if (!rejects("")) {
          return false;
        }
        if (!rejects(".")) {
          return false;
        }
        if (!rejects("..")) {
          return false;
        }
        if (!rejects("dir/")) {
          return false;
        }
        if (!rejects("sdmc:evil")) {
          return false;
        }
        if (!rejects("C:evil")) {
          return false;
        }
        if (!rejects("bad*name")) {
          return false;
        }
        if (!rejects("bad?name")) {
          return false;
        }
        if (!rejects("bad|name")) {
          return false;
        }
        if (!rejects("bad\"name")) {
          return false;
        }
        if (!rejects("bad<name")) {
          return false;
        }
        if (!rejects("bad>name")) {
          return false;
        }
        if (!rejects("ctrl\x01name")) {
          return false;
        }
        if (!rejects("del\x7Fname")) {
          return false;
        }
        const std::string overlong(cfg::INSTALL_NAME_MAX + 1, 'x');
        return sanitize_entry_name(overlong).empty();
      }
    static_assert(sanitize_entry_name_is_safe(),
      "Archive entry names must be reduced to a safe bare filename.");
    [
      [nodiscard]
    ] bool extension_supported(std::string_view file_name) noexcept {
      return ends_with_ci(file_name, ".zip") ||
        ends_with_ci(file_name, ".7z") ||
        ends_with_ci(file_name, ".rar");
    }
    std::atomic < bool > quit_requested {
      false
    };
    std::atomic < bool > cancel_requested {
      false
    };
    std::atomic < int > percent {
      -1
    };
    std::atomic < int64_t > bytes_done {
      0
    };
    std::atomic < int > files_written {
      0
    };
    [
      [nodiscard]
    ] bool aborting() noexcept {
      return quit_requested.load(std::memory_order_relaxed) ||
        cancel_requested.load(std::memory_order_relaxed);
    }
    struct ExtractResult {
      std::vector < std::string > files;
      std::string message;
    };
    class ArchiveReader {
      public: ArchiveReader() noexcept: handle_ {
        archive_read_new()
      } {}
      ArchiveReader(const ArchiveReader & ) = delete;
      ArchiveReader & operator = (const ArchiveReader & ) = delete;
      ~ArchiveReader() {
        if (handle_ != nullptr) {
          archive_read_free(handle_);
        }
      }
      [
        [nodiscard]
      ] struct archive * get() const noexcept {
          return handle_;
        }
        [
          [nodiscard]
        ] explicit operator bool() const noexcept {
          return handle_ != nullptr;
        }
      private: struct archive * handle_ = nullptr;
    };
    [
      [nodiscard]
    ] std::string archive_message(struct archive * handle, std::string_view what) {
        const char * detail = handle != nullptr ? archive_error_string(handle) : nullptr;
        if (detail != nullptr && detail[0] != '\0') {
          return fmt::format("{}: {}", what, detail);
        }
        return fmt::format("{}.", what);
      }
      [
        [nodiscard]
      ] bool write_zeros(std::FILE * file, int64_t count) noexcept {
        constexpr std::size_t CHUNK = 512;
        const std::array < char, CHUNK > zeros {};
        while (count > 0) {
          const auto chunk = static_cast < std::size_t > (std::min < int64_t > (count, CHUNK));
          if (std::fwrite(zeros.data(), 1, chunk, file) != chunk) {
            return false;
          }
          count -= static_cast < int64_t > (chunk);
        }
        return true;
      }
    void publish_progress(struct archive * handle, int64_t archive_bytes,
        std::size_t file_count) noexcept {
        files_written.store(static_cast < int > (file_count), std::memory_order_relaxed);
        if (archive_bytes <= 0) {
          return;
        }
        const la_int64_t consumed = archive_filter_bytes(handle, -1);
        if (consumed < 0) {
          return;
        }
        const int64_t ratio = (static_cast < int64_t > (consumed) * 100) / archive_bytes;
        percent.store(static_cast < int > (std::clamp < int64_t > (ratio, 0, 100)),
          std::memory_order_relaxed);
      }
      [
        [nodiscard]
      ] std::size_t extract(const char * archive_path, ExtractResult & out) {
        ArchiveReader reader;
        if (!reader) {
          out.message = "Out of memory.";
          return 0;
        }
        archive_read_support_format_zip(reader.get());
        archive_read_support_format_7zip(reader.get());
        archive_read_support_format_rar(reader.get());
        archive_read_support_format_rar5(reader.get());
        const int64_t archive_bytes = sd::file_size(archive_path).value_or(0);
        {
          const sd::PathGuard guard {
            sd::path_lock
          };
          if (archive_read_open_filename(reader.get(), archive_path,
              cfg::ARCHIVE_BLOCK_SIZE) != ARCHIVE_OK) {
            out.message = archive_message(reader.get(), "Cannot open archive");
            return 0;
          }
        }
        sys::ScopeGuard discard {
          [ & out] {
            out.files.clear();
          }
        };
        int64_t total_bytes = 0;
        int entries = 0;
        for (;;) {
          if (aborting())[[unlikely]] {
            out.message = "Cancelled.";
            return 0;
          }
          if (++entries > cfg::INSTALL_MAX_ENTRIES) {
            out.message = "Archive has too many entries.";
            return 0;
          }
          struct archive_entry * entry = nullptr;
          const int header = archive_read_next_header(reader.get(), & entry);
          if (header == ARCHIVE_EOF) {
            break;
          }
          if (header == ARCHIVE_RETRY) {
            continue;
          }
          if (header < ARCHIVE_WARN) {
            out.message = archive_message(reader.get(), "Archive error");
            return 0;
          }
          publish_progress(reader.get(), archive_bytes, out.files.size());
          if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;
          }
          if (archive_entry_is_encrypted(entry)) {
            out.message = "Archive is password protected.";
            return 0;
          }
          const char * raw = archive_entry_pathname_utf8(entry);
          if (raw == nullptr) {
            raw = archive_entry_pathname(entry);
          }
          if (raw == nullptr) {
            continue;
          }
          if (!ends_with_ci(raw, cfg::CHPACK_SUFFIX)) {
            continue;
          }
          const std::string name = sanitize_entry_name(raw);
          if (name.empty()) {
            continue;
          }
          const bool already_recorded = std::ranges::any_of(out.files, [ & name](const std::string & seen) {
            return equals_ci(seen, name);
          });
          if (!already_recorded && out.files.size() >= cfg::INSTALL_MAX_FILES) {
            out.message = "Archive has too many .chpack files.";
            return 0;
          }
          const std::string destination = fmt::format("{}{}", cfg::CTGP7_DIR.view(), name);
          const std::string partial = destination + ".part";
          sys::FileHandle file = sd::open(partial.c_str(), "wb");
          if (!file) {
            out.message = "Cannot write to the SD card.";
            return 0;
          }
          sys::ScopeGuard remove_partial {
            [ & partial] {
              sd::unlink_quietly(partial.c_str());
            }
          };
          int64_t expected_offset = 0;
          for (;;) {
            const void * block = nullptr;
            std::size_t block_size = 0;
            la_int64_t block_offset = 0;
            const int status = archive_read_data_block(reader.get(), & block, &
              block_size, & block_offset);
            if (status == ARCHIVE_EOF) {
              break;
            }
            if (status < ARCHIVE_WARN) {
              out.message = archive_message(reader.get(), "Extract failed");
              return 0;
            }
            if (block_size == 0) {
              continue;
            }
            if (aborting())[[unlikely]] {
              out.message = "Cancelled.";
              return 0;
            }
            const auto offset = static_cast < int64_t > (block_offset);
            if (offset < expected_offset) {
              out.message = "Corrupt archive entry.";
              return 0;
            }
            if (offset > expected_offset) {
              if (!write_zeros(file.get(), offset - expected_offset)) {
                out.message = "SD write failed.";
                return 0;
              }
              expected_offset = offset;
            }
            expected_offset += static_cast < int64_t > (block_size);
            total_bytes += static_cast < int64_t > (block_size);
            if (expected_offset > cfg::INSTALL_MAX_FILE_BYTES ||
              total_bytes > cfg::INSTALL_MAX_TOTAL_BYTES) {
              out.message = "Archive is too large.";
              return 0;
            }
            if (std::fwrite(block, 1, block_size, file.get()) != block_size) {
              out.message = "SD write failed (card full?).";
              return 0;
            }
            bytes_done.store(total_bytes, std::memory_order_relaxed);
            publish_progress(reader.get(), archive_bytes, out.files.size());
          }
          if (!file.close()) {
            out.message = "SD write failed.";
            return 0;
          }
          if (!sd::replace_file(partial.c_str(), destination.c_str())) {
            out.message = "Cannot replace the installed file.";
            return 0;
          }
          remove_partial.dismiss();
          if (!already_recorded) {
            out.files.push_back(name);
          }
          publish_progress(reader.get(), archive_bytes, out.files.size());
        }
        discard.dismiss();
        return out.files.size();
      }
    struct DownloadSink {
      std::FILE * file = nullptr;
      curl_off_t written = 0;
    };
    std::size_t download_write(void * contents, std::size_t size, std::size_t nmemb, void * userp) {
      auto * sink = static_cast < DownloadSink * > (userp);
      const std::size_t total = size * nmemb;
      if (total == 0) {
        return 0;
      }
      if (sink -> written > cfg::DOWNLOAD_MAX_BYTES - static_cast < curl_off_t > (total)) {
        return CURL_WRITEFUNC_ERROR;
      }
      if (std::fwrite(contents, 1, total, sink -> file) != total) {
        return CURL_WRITEFUNC_ERROR;
      }
      sink -> written += static_cast < curl_off_t > (total);
      return total;
    }
    int download_progress(void * , curl_off_t download_total, curl_off_t download_now,
      curl_off_t, curl_off_t) {
      if (aborting()) {
        return 1;
      }
      percent.store(download_total > 0 ?
        static_cast < int > ((download_now * 100) / download_total) :
        -1,
        std::memory_order_relaxed);
      bytes_done.store(static_cast < int64_t > (download_now), std::memory_order_relaxed);
      return 0;
    }
    void configure_download(CURL * curl) noexcept {
        net::configure(curl);
        curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, & download_write);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg::DL_CONNECT_TIMEOUT);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg::DL_LOW_SPEED_LIMIT);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, cfg::DL_LOW_SPEED_TIME);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, cfg::DOWNLOAD_MAX_BYTES);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, & download_progress);
      }
      [
        [nodiscard]
      ] bool download(CURL * curl,
        const std::string & url, std::string & message) {
        sd::unlink_quietly(cfg::DOWNLOAD_TMP.c_str());
        sys::FileHandle file = sd::open(cfg::DOWNLOAD_TMP.c_str(), "wb");
        if (!file) {
          message = "Cannot write to the SD card.";
          return false;
        }
        DownloadSink sink {
          file.get(), 0
        };
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, & sink);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
        const CURLcode outcome = curl_easy_perform(curl);
        const bool closed = file.close();
        if (outcome != CURLE_OK) {
          switch (outcome) {
          case CURLE_ABORTED_BY_CALLBACK:
            message = "Cancelled.";
            break;
          case CURLE_OPERATION_TIMEDOUT:
            message = "Download stalled.";
            break;
          case CURLE_WRITE_ERROR:
            message = "Download too large, or the card is full.";
            break;
          default:
            message = fmt::format("Download failed: {}", curl_easy_strerror(outcome));
            break;
          }
          return false;
        }
        if (!closed) {
          message = "SD write failed.";
          return false;
        }
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, & code) != CURLE_OK ||
          code < 200 || code >= 300) {
          message = fmt::format("Server returned HTTP {}.", code);
          return false;
        }
        curl_off_t reported = 0;
        if (curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, & reported) == CURLE_OK &&
          reported != sink.written) {
          message = "Download was interrupted.";
          return false;
        }
        if (sink.written <= 0) {
          message = "The server sent an empty file.";
          return false;
        }
        return true;
      }
    enum class Phase {
      IDLE,
      DOWNLOADING,
      EXTRACTING,
      FINISHING
    };
    enum class Slot {
      EMPTY,
      REQUESTED,
      RUNNING,
      COMPLETE
    };
    struct Request {
      int mod_id = 0;
      int64_t file_date = 0;
      std::string url;
      std::string source_name;
    };
    struct Result {
      int mod_id = 0;
      int64_t file_date = 0;
      std::string source_name;
      std::vector < std::string > files;
      bool ok = false;
      std::string message;
    };
    sys::Mutex mailbox_lock;
    sys::Event wake {
      true
    };
    sys::Thread worker;
    Slot slot = Slot::EMPTY;
    Request pending;
    Result finished;
    std::atomic < Phase > phase {
      Phase::IDLE
    };
    bool ready = false;
    std::string user_message;
    [
      [nodiscard]
    ] bool busy() {
      if (!ready) {
        return false;
      }
      const std::scoped_lock guard {
        mailbox_lock
      };
      return slot == Slot::REQUESTED || slot == Slot::RUNNING;
    }
    void cancel() {
      if (busy()) {
        cancel_requested.store(true, std::memory_order_relaxed);
      }
    }
    void worker_main(void * ) {
        sys::CurlHandle curl;
        if (curl) {
          configure_download(curl.get());
        }
        for (;;) {
          wake.clear();
          Request request;
          bool have = false;
          {
            const std::scoped_lock guard {
              mailbox_lock
            };
            if (!quit_requested.load(std::memory_order_relaxed) && slot == Slot::REQUESTED) {
              request = pending;
              slot = Slot::RUNNING;
              have = true;
            }
          }
          if (quit_requested.load(std::memory_order_relaxed)) {
            break;
          }
          if (!have) {
            wake.wait_for(cfg::INSTALL_IDLE_WAIT_NS);
            continue;
          }
          Result result;
          result.mod_id = request.mod_id;
          result.file_date = request.file_date;
          result.source_name = request.source_name;
          phase.store(Phase::DOWNLOADING, std::memory_order_relaxed);
          percent.store(-1, std::memory_order_relaxed);
          bytes_done.store(0, std::memory_order_relaxed);
          if (!curl) {
            result.message = "Network is unavailable.";
          } else if (download(curl.get(), request.url, result.message)) {
            phase.store(Phase::EXTRACTING, std::memory_order_relaxed);
            percent.store(-1, std::memory_order_relaxed);
            bytes_done.store(0, std::memory_order_relaxed);
            files_written.store(0, std::memory_order_relaxed);
            ExtractResult extracted;
            if (extract(cfg::DOWNLOAD_TMP.c_str(), extracted) > 0) {
              result.files = std::move(extracted.files);
              result.ok = true;
            } else if (!extracted.message.empty()) {
              result.message = std::move(extracted.message);
            } else {
              result.message = "No .chpack files.";
            }
          }
          sd::unlink_quietly(cfg::DOWNLOAD_TMP.c_str());
          phase.store(Phase::FINISHING, std::memory_order_relaxed);
          {
            const std::scoped_lock guard {
              mailbox_lock
            };
            finished = std::move(result);
            slot = Slot::COMPLETE;
          }
        }
      }
      [
        [nodiscard]
      ] bool begin(const store::ModData & mod) {
        if (!ready || busy()) {
          return false;
        }
        if (mod.latest_file_url.empty()) {
          user_message = "This mod has no download link.";
          return false;
        }
        if (!extension_supported(mod.latest_file_name)) {
          user_message = "Unsupported archive type (only .zip, .7z and .rar).";
          return false;
        }
        Request request;
        request.mod_id = mod.id;
        request.file_date = mod.latest_file_date;
        request.url = mod.latest_file_url;
        request.source_name = mod.latest_file_name;
        cancel_requested.store(false, std::memory_order_relaxed);
        percent.store(-1, std::memory_order_relaxed);
        bytes_done.store(0, std::memory_order_relaxed);
        files_written.store(0, std::memory_order_relaxed);
        phase.store(Phase::DOWNLOADING, std::memory_order_relaxed);
        user_message.clear();
        bool accepted = false;
        {
          const std::scoped_lock guard {
            mailbox_lock
          };
          if (slot == Slot::EMPTY) {
            pending = std::move(request);
            slot = Slot::REQUESTED;
            accepted = true;
          }
        }
        if (accepted) {
          wake.signal();
        }
        return accepted;
      }
    void apply(const Result & result) {
      if (const store::InstallRecord * previous = store::installed.find(result.mod_id)) {
        for (const std::string & old_file: previous -> files) {
          const bool still_supplied = std::ranges::any_of(
            result.files, [ & old_file](const std::string & current) {
              return equals_ci(old_file, current);
            });
          if (still_supplied) {
            continue;
          }
          const std::string path = fmt::format("{}{}", cfg::CTGP7_DIR.view(), old_file);
          sd::unlink_quietly(path.c_str());
        }
      }
      store::InstallRecord record;
      record.date = result.file_date;
      record.files = result.files;
      record.source_file_name = result.source_name;
      store::installed.insert_or_assign(result.mod_id, std::move(record));
      (void) store::save_installed();
      model::resort_after_change();
    }
    void tick() {
        if (!ready) {
          return;
        }
        Result result;
        bool have = false;
        {
          const std::scoped_lock guard {
            mailbox_lock
          };
          if (slot == Slot::COMPLETE) {
            result = std::move(finished);
            finished = Result {};
            slot = Slot::EMPTY;
            have = true;
          }
        }
        if (!have) {
          return;
        }
        phase.store(Phase::IDLE, std::memory_order_relaxed);
        cancel_requested.store(false, std::memory_order_relaxed);
        if (result.ok) {
          apply(result);
          user_message.clear();
          return;
        }
        user_message = result.message.empty() ? "Install failed." : std::move(result.message);
      }
      [
        [nodiscard]
      ] std::string progress_label() {
        switch (phase.load(std::memory_order_relaxed)) {
        case Phase::EXTRACTING: {
          const int done = percent.load(std::memory_order_relaxed);
          const int files = files_written.load(std::memory_order_relaxed);
          if (done < 0) {
            return "Extracting...";
          }
          if (files > 0) {
            return fmt::format("Extracting {}% ({} file{})", done, files,
              files == 1 ? "" : "s");
          }
          return fmt::format("Extracting {}%", done);
        }
        case Phase::FINISHING:
          return "Finishing...";
        case Phase::IDLE:
        case Phase::DOWNLOADING:
          break;
        }
        const int done = percent.load(std::memory_order_relaxed);
        if (done >= 0) {
          return fmt::format("Downloading {}%", std::min(done, 100));
        }
        const int64_t kilobytes = bytes_done.load(std::memory_order_relaxed) / 1024;
        if (kilobytes >= 1024) {
          const int64_t whole = kilobytes / 1024;
          const int64_t tenths = ((kilobytes % 1024) * 10) / 1024;
          return fmt::format("Downloading {}.{} MB", whole, tenths);
        }
        return fmt::format("Downloading {} KB", kilobytes);
      }
    void do_action() {
      if (busy()) {
        return;
      }
      const store::ModData * mod = model::selected_mod();
      if (mod == nullptr) {
        return;
      }
      const model::ModAction action = model::current_action();
      if (action == model::ModAction::INSTALL || action == model::ModAction::UPDATE) {
        (void) begin( * mod);
      }
    }
    void uninstall() {
      if (busy()) {
        return;
      }
      const store::ModData * mod = model::selected_mod();
      if (mod == nullptr) {
        return;
      }
      const store::InstallRecord * record = store::installed.find(mod -> id);
      if (record == nullptr) {
        return;
      }
      const std::vector < std::string > files = record -> files;
      const int mod_id = mod -> id;
      store::installed.erase(mod_id);
      (void) store::save_installed();
      for (const std::string & file: files) {
        const std::string path = fmt::format("{}{}", cfg::CTGP7_DIR.view(), file);
        sd::unlink_quietly(path.c_str());
      }
      user_message.clear();
      model::resort_after_change();
    }
    bool init() {
      if (ready) {
        return true;
      }
      quit_requested.store(false, std::memory_order_relaxed);
      cancel_requested.store(false, std::memory_order_relaxed);
      slot = Slot::EMPTY;
      sd::unlink_quietly(cfg::DOWNLOAD_TMP.c_str());
      worker = sys::Thread::spawn( & worker_main, nullptr, cfg::INSTALL_STACK_SIZE,
        cfg::WORKER_PRIORITY, cfg::ANY_CORE);
      if (!worker) {
        return false;
      }
      ready = true;
      return true;
    }
    void shutdown() {
      if (!ready) {
        return;
      }
      ready = false;
      quit_requested.store(true, std::memory_order_relaxed);
      wake.signal();
      worker.join();
      sd::unlink_quietly(cfg::DOWNLOAD_TMP.c_str());
    }
  }
  namespace thumbs {
    struct RawImage {
      unsigned char * rgb = nullptr;
      int width = 0;
      int height = 0;
    };
    struct JpegError {
      jpeg_error_mgr pub;
      jmp_buf jump;
    };
    void jpeg_error_exit(j_common_ptr info) {
      longjmp(reinterpret_cast < JpegError * > (info -> err) -> jump, 1);
    }
    void jpeg_silence(j_common_ptr) {}
      [
        [nodiscard]
      ] constexpr unsigned pick_scale_numerator(unsigned width, unsigned height) noexcept {
        for (unsigned numerator = 1; numerator < 8; ++numerator) {
          if ((width * numerator + 7u) / 8u >= static_cast < unsigned > (cfg::THUMB_IMG_W) &&
            (height * numerator + 7u) / 8u >= static_cast < unsigned > (cfg::THUMB_IMG_H)) {
            return numerator;
          }
        }
        return 8;
      }
    static_assert(pick_scale_numerator(220, 124) == 4,
      "220x124 should decode at 4/8 scale.");
    static_assert(pick_scale_numerator(110, 62) == 8,
      "A 110x62 source must not be downscaled.");
    [
      [nodiscard]
    ] bool jpeg_decode(const unsigned char * source, std::size_t length, RawImage & out) {
        jpeg_decompress_struct cinfo;
        JpegError jerr;
        unsigned char * volatile rgb = nullptr;
        volatile bool ok = false;
        volatile int width = 0;
        volatile int height = 0;
        cinfo.err = jpeg_std_error( & jerr.pub);
        jerr.pub.error_exit = & jpeg_error_exit;
        jerr.pub.output_message = & jpeg_silence;
        if (setjmp(jerr.jump) == 0) {
          jpeg_create_decompress( & cinfo);
          jpeg_mem_src( & cinfo, source, static_cast < unsigned long > (length));
          if (jpeg_read_header( & cinfo, TRUE) == JPEG_HEADER_OK &&
            cinfo.image_width > 0 &&
            cinfo.image_width <= cfg::THUMB_SRC_MAX_DIM &&
            cinfo.image_height > 0 &&
            cinfo.image_height <= cfg::THUMB_SRC_MAX_DIM) {
            cinfo.scale_num = pick_scale_numerator(cinfo.image_width, cinfo.image_height);
            cinfo.scale_denom = 8;
            cinfo.out_color_space = JCS_RGB;
            cinfo.dct_method = JDCT_IFAST;
            cinfo.do_fancy_upsampling = FALSE;
            jpeg_calc_output_dimensions( & cinfo);
            const std::size_t pixels =
              static_cast < std::size_t > (cinfo.output_width) * cinfo.output_height;
            if (cinfo.output_components == 3 && pixels <= cfg::THUMB_SRC_MAX_PIXELS) {
              rgb = static_cast < unsigned char * > (std::malloc(pixels * 3u));
              if (rgb != nullptr) {
                jpeg_start_decompress( & cinfo);
                while (cinfo.output_scanline < cinfo.output_height) {
                  JSAMPROW row =
                    rgb + static_cast < std::size_t > (cinfo.output_scanline) *
                    static_cast < std::size_t > (cinfo.output_width) * 3u;
                  if (jpeg_read_scanlines( & cinfo, & row, 1) != 1) {
                    break;
                  }
                }
                ok = cinfo.output_scanline >= cinfo.output_height;
                width = static_cast < int > (cinfo.output_width);
                height = static_cast < int > (cinfo.output_height);
                jpeg_finish_decompress( & cinfo);
              }
            }
          }
        }
        jpeg_destroy_decompress( & cinfo);
        if (!ok) {
          std::free(rgb);
          return false;
        }
        out.rgb = rgb;
        out.width = width;
        out.height = height;
        return true;
      }
      [
        [nodiscard]
      ] bool jpeg_encode(const unsigned char * rgb, unsigned char ** out_buffer,
        unsigned long * out_length) {
        jpeg_compress_struct cinfo;
        JpegError jerr;
        unsigned char * buffer = nullptr;
        unsigned long length = 0;
        volatile bool ok = false;
        cinfo.err = jpeg_std_error( & jerr.pub);
        jerr.pub.error_exit = & jpeg_error_exit;
        jerr.pub.output_message = & jpeg_silence;
        if (setjmp(jerr.jump) == 0) {
          jpeg_create_compress( & cinfo);
          jpeg_mem_dest( & cinfo, & buffer, & length);
          cinfo.image_width = static_cast < JDIMENSION > (cfg::THUMB_IMG_W);
          cinfo.image_height = static_cast < JDIMENSION > (cfg::THUMB_IMG_H);
          cinfo.input_components = 3;
          cinfo.in_color_space = JCS_RGB;
          jpeg_set_defaults( & cinfo);
          jpeg_set_quality( & cinfo, cfg::THUMB_JPEG_QUALITY, TRUE);
          jpeg_start_compress( & cinfo, TRUE);
          while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row =
              const_cast < JSAMPROW > (
                rgb + static_cast < std::size_t > (cinfo.next_scanline) *
                static_cast < std::size_t > (cfg::THUMB_IMG_W) * 3u);
            if (jpeg_write_scanlines( & cinfo, & row, 1) != 1) {
              break;
            }
          }
          if (cinfo.next_scanline >= cinfo.image_height) {
            jpeg_finish_compress( & cinfo);
            ok = true;
          }
        }
        jpeg_destroy_compress( & cinfo);
        if (!ok || buffer == nullptr) {
          std::free(buffer);
          return false;
        }
        * out_buffer = buffer;
        * out_length = length;
        return true;
      }
    void crop_resize(const RawImage & source, unsigned char * destination) noexcept {
        const int source_width = source.width;
        const int source_height = source.height;
        int crop_width = 0;
        int crop_height = 0;
        if (source_width * cfg::THUMB_IMG_H >= source_height * cfg::THUMB_IMG_W) {
          crop_width = (source_height * cfg::THUMB_IMG_W) / cfg::THUMB_IMG_H;
          crop_height = source_height;
        } else {
          crop_width = source_width;
          crop_height = (source_width * cfg::THUMB_IMG_H) / cfg::THUMB_IMG_W;
        }
        crop_width = std::max(crop_width, 1);
        crop_height = std::max(crop_height, 1);
        int crop_x = std::max((source_width - crop_width) / 2, 0);
        crop_width = std::min(crop_width, source_width - crop_x);
        crop_height = std::min(crop_height, source_height);
        for (int y = 0; y < cfg::THUMB_IMG_H; ++y) {
          int y0 = (y * crop_height) / cfg::THUMB_IMG_H;
          int y1 = ((y + 1) * crop_height) / cfg::THUMB_IMG_H;
          if (y1 <= y0) {
            y1 = y0 + 1;
          }
          y1 = std::min(y1, crop_height);
          for (int x = 0; x < cfg::THUMB_IMG_W; ++x) {
            int x0 = crop_x + (x * crop_width) / cfg::THUMB_IMG_W;
            int x1 = crop_x + ((x + 1) * crop_width) / cfg::THUMB_IMG_W;
            if (x1 <= x0) {
              x1 = x0 + 1;
            }
            x1 = std::min(x1, crop_x + crop_width);
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            unsigned samples = 0;
            for (int sy = y0; sy < y1; ++sy) {
              const unsigned char * pixel =
                source.rgb + (static_cast < std::size_t > (sy) *
                  static_cast < std::size_t > (source_width) +
                  static_cast < std::size_t > (x0)) *
                3u;
              for (int sx = x0; sx < x1; ++sx, pixel += 3) {
                red += pixel[0];
                green += pixel[1];
                blue += pixel[2];
                ++samples;
              }
            }
            unsigned char * out =
              destination + (static_cast < std::size_t > (y) *
                static_cast < std::size_t > (cfg::THUMB_IMG_W) +
                static_cast < std::size_t > (x)) *
              3u;
            out[0] = static_cast < unsigned char > (red / samples);
            out[1] = static_cast < unsigned char > (green / samples);
            out[2] = static_cast < unsigned char > (blue / samples);
          }
        }
      }
      [
        [nodiscard]
      ] constexpr u32 tiled_index(u32 x, u32 y) noexcept {
        const u32 tile_row = ((y >> 3) * (cfg::THUMB_TEX_W >> 3)) << 6;
        const u32 tile_col = (x >> 3) << 6;
        const u32 x_bits = (x & 1u) | ((x & 2u) << 1) | ((x & 4u) << 2);
        const u32 y_bits = ((y & 1u) << 1) | ((y & 2u) << 2) | ((y & 4u) << 3);
        return tile_row + tile_col + x_bits + y_bits;
      }
      [
        [nodiscard]
      ] consteval bool tiled_index_is_bijective() {
        constexpr std::size_t TEXELS =
          static_cast < std::size_t > (cfg::THUMB_TEX_W) * static_cast < std::size_t > (cfg::THUMB_TEX_H);
        std::array < bool, TEXELS > seen {};
        for (u32 y = 0; y < cfg::THUMB_TEX_H; ++y) {
          for (u32 x = 0; x < cfg::THUMB_TEX_W; ++x) {
            const u32 index = tiled_index(x, y);
            if (index >= TEXELS || seen[index]) {
              return false;
            }
            seen[index] = true;
          }
        }
        return true;
      }
    static_assert(tiled_index_is_bijective(),
      "The tiled texture index must cover every texel exactly once.");
    void swizzle_rgb565(const unsigned char * source, int source_width, int source_height,
      u16 * destination) noexcept {
      for (u32 y = 0; y < cfg::THUMB_TEX_H; ++y) {
        const u32 sy = y < static_cast < u32 > (source_height) ?
          y :
          static_cast < u32 > (source_height - 1);
        const unsigned char * row =
          source + static_cast < std::size_t > (sy) *
          static_cast < std::size_t > (source_width) * 3u;
        for (u32 x = 0; x < cfg::THUMB_TEX_W; ++x) {
          const u32 sx = x < static_cast < u32 > (source_width) ?
            x :
            static_cast < u32 > (source_width - 1);
          const unsigned char * pixel = row + static_cast < std::size_t > (sx) * 3u;
          const auto texel = static_cast < u16 > (
            ((static_cast < u32 > (pixel[0]) & 0xF8u) << 8) |
            ((static_cast < u32 > (pixel[1]) & 0xFCu) << 3) |
            (static_cast < u32 > (pixel[2]) >> 3));
          destination[tiled_index(x, y)] = texel;
        }
      }
    }
    Tex3DS_SubTexture sub_texture {};
    void build_sub_texture() noexcept {
      const float scale_x = cfg::CONTENT_W / static_cast < float > (cfg::THUMB_IMG_W);
      const float scale_y = cfg::THUMB_H / static_cast < float > (cfg::THUMB_IMG_H);
      const float scale = std::max(scale_x, scale_y);
      const float visible_w =
        std::min(cfg::CONTENT_W / scale, static_cast < float > (cfg::THUMB_IMG_W));
      const float visible_h =
        std::min(cfg::THUMB_H / scale, static_cast < float > (cfg::THUMB_IMG_H));
      const float origin_x =
        std::max((static_cast < float > (cfg::THUMB_IMG_W) - visible_w) * 0.5f, 0.0f);
      const float origin_y =
        std::max((static_cast < float > (cfg::THUMB_IMG_H) - visible_h) * 0.5f, 0.0f);
      sub_texture.width = static_cast < u16 > (cfg::CONTENT_W);
      sub_texture.height = static_cast < u16 > (cfg::THUMB_H);
      sub_texture.left = origin_x / static_cast < float > (cfg::THUMB_TEX_W);
      sub_texture.right = (origin_x + visible_w) / static_cast < float > (cfg::THUMB_TEX_W);
      sub_texture.top = 1.0f - origin_y / static_cast < float > (cfg::THUMB_TEX_H);
      sub_texture.bottom =
        1.0f - (origin_y + visible_h) / static_cast < float > (cfg::THUMB_TEX_H);
    }
    struct Buffer {
      sys::MallocArray < unsigned char > data;
      std::size_t length = 0;
    };
    [
      [nodiscard]
    ] std::string cache_path(int mod_id) {
        return fmt::format("{}{}.jpg", cfg::THUMB_DIR.view(), mod_id);
      }
      [
        [nodiscard]
      ] std::optional < Buffer > read_cached(const char * path) {
        sys::FileHandle file = sd::open(path, "rb");
        if (!file) {
          return std::nullopt;
        }
        if (std::fseek(file.get(), 0, SEEK_END) != 0) {
          return std::nullopt;
        }
        const long size = std::ftell(file.get());
        if (size <= 0 || static_cast < std::size_t > (size) > cfg::THUMB_MAX_BYTES) {
          return std::nullopt;
        }
        std::rewind(file.get());
        const auto length = static_cast < std::size_t > (size);
        Buffer buffer {
          sys::MallocArray < unsigned char > {
              static_cast < unsigned char * > (std::malloc(length))
            },
            length
        };
        if (!buffer.data) {
          return std::nullopt;
        }
        if (std::fread(buffer.data.get(), 1, length, file.get()) != length) {
          return std::nullopt;
        }
        return buffer;
      }
      [
        [nodiscard]
      ] bool write_cached(const std::string & path,
        const void * data, std::size_t length) {
        const std::string temporary = path + ".tmp";
        {
          sys::FileHandle file = sd::open(temporary.c_str(), "wb");
          if (!file) {
            return false;
          }
          const bool wrote = std::fwrite(data, 1, length, file.get()) == length;
          const bool closed = file.close();
          if (!wrote || !closed) {
            sd::unlink_quietly(temporary.c_str());
            return false;
          }
        }
        return sd::replace_file(temporary.c_str(), path.c_str());
      }
    struct DownloadBuffer {
      unsigned char * data = nullptr;
      std::size_t length = 0;
      std::size_t capacity = 0;
    };
    std::atomic < bool > quit_requested {
      false
    };
    std::size_t download_write(void * contents, std::size_t size, std::size_t nmemb, void * userp) {
      auto * sink = static_cast < DownloadBuffer * > (userp);
      const std::size_t total = size * nmemb;
      if (total == 0) {
        return 0;
      }
      if (sink -> length + total > cfg::THUMB_MAX_BYTES) {
        return 0;
      }
      if (sink -> length + total > sink -> capacity) {
        std::size_t capacity = sink -> capacity != 0 ? sink -> capacity : 32768;
        while (capacity < sink -> length + total) {
          capacity *= 2;
        }
        auto * grown = static_cast < unsigned char * > (std::realloc(sink -> data, capacity));
        if (grown == nullptr) {
          return 0;
        }
        sink -> data = grown;
        sink -> capacity = capacity;
      }
      std::memcpy(sink -> data + sink -> length, contents, total);
      sink -> length += total;
      return total;
    }
    int download_abort(void * , curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      return quit_requested.load(std::memory_order_relaxed) ? 1 : 0;
    }
    void configure_download(CURL * curl) noexcept {
        net::configure(curl);
        curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, & download_write);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, & download_abort);
      }
      [
        [nodiscard]
      ] bool produce(CURL * curl, int mod_id,
        const char * url, u16 * destination) {
        const std::string path = cache_path(mod_id);
        RawImage image {};
        bool from_disk = false;
        if (std::optional < Buffer > cached = read_cached(path.c_str())) {
          if (jpeg_decode(cached -> data.get(), cached -> length, image)) {
            from_disk = true;
          } else {
            sd::unlink_quietly(path.c_str());
          }
        }
        if (!from_disk) {
          if (curl == nullptr || url[0] == '\0' ||
            quit_requested.load(std::memory_order_relaxed)) {
            return false;
          }
          DownloadBuffer sink;
          curl_easy_setopt(curl, CURLOPT_URL, url);
          curl_easy_setopt(curl, CURLOPT_WRITEDATA, & sink);
          const CURLcode outcome = curl_easy_perform(curl);
          const sys::MallocArray < unsigned char > downloaded {
            sink.data
          };
          if (outcome != CURLE_OK) {
            return false;
          }
          long code = 0;
          if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, & code) != CURLE_OK) {
            return false;
          }
          if (code < 200 || code >= 300 || sink.length < 3) {
            return false;
          }
          if (sink.data[0] != 0xFF || sink.data[1] != 0xD8 || sink.data[2] != 0xFF) {
            return false;
          }
          if (!jpeg_decode(sink.data, sink.length, image)) {
            return false;
          }
        }
        const sys::MallocArray < unsigned char > decoded {
          image.rgb
        };
        constexpr std::size_t SCALED_BYTES =
          static_cast < std::size_t > (cfg::THUMB_IMG_W) *
          static_cast < std::size_t > (cfg::THUMB_IMG_H) * 3u;
        const sys::MallocArray < unsigned char > scaled {
          static_cast < unsigned char * > (std::malloc(SCALED_BYTES))
        };
        if (!scaled) {
          return false;
        }
        crop_resize(image, scaled.get());
        if (!from_disk) {
          unsigned char * encoded = nullptr;
          unsigned long encoded_length = 0;
          if (jpeg_encode(scaled.get(), & encoded, & encoded_length)) {
            const sys::MallocArray < unsigned char > owned {
              encoded
            };
            (void) write_cached(path, encoded, static_cast < std::size_t > (encoded_length));
          }
        }
        swizzle_rgb565(scaled.get(), cfg::THUMB_IMG_W, cfg::THUMB_IMG_H, destination);
        GSPGPU_FlushDataCache(destination, cfg::THUMB_TEX_BYTES);
        return true;
      }
    struct TextureSlot {
      C3D_Tex texture {};
      int mod_id = 0;
      u32 last_used = 0;
    };
    struct Request {
      int mod_id = 0;
      std::array < char, cfg::THUMB_URL_MAX > url {};
    };
    struct Publication {
      sys::LinearBuffer pixels;
      sys::Event ack {
        true
      };
      int mod_id = 0;
      bool ready = false;
      bool ok = false;
    };
    struct WorkerContext {
      int index = 0;
    };
    sys::Mutex lock;
    sys::Event wake {
      true
    };
    std::array < TextureSlot, cfg::THUMB_SLOTS > slots;
    std::array < Publication, cfg::THUMB_WORKERS > publications;
    std::array < sys::Thread, cfg::THUMB_WORKERS > workers;
    std::array < WorkerContext, cfg::THUMB_WORKERS > worker_contexts;
    std::array < Request, cfg::THUMB_QUEUE > queue;
    int queue_size = 0;
    std::array < int, cfg::THUMB_WORKERS > in_flight {};
    std::array < int, cfg::THUMB_FAIL_RING > failures {};
    int failure_cursor = 0;
    std::array < int, cfg::CARDS_PER_PAGE > wanted {};
    int wanted_count = 0;
    u32 frame_counter = 0;
    bool ready = false;
    [
      [nodiscard]
    ] bool is_wanted(int mod_id) noexcept {
        return std::ranges::find(wanted.begin(), wanted.begin() + wanted_count, mod_id) !=
          wanted.begin() + wanted_count;
      }
      [
        [nodiscard]
      ] bool is_resident(int mod_id) noexcept {
        return std::ranges::any_of(slots, [mod_id](const TextureSlot & slot) {
          return slot.mod_id == mod_id;
        });
      }
      [
        [nodiscard]
      ] bool has_failed(int mod_id) noexcept {
        return std::ranges::find(failures, mod_id) != failures.end();
      }
    void mark_failed(int mod_id) noexcept {
        if (mod_id == 0 || has_failed(mod_id)) {
          return;
        }
        failures[static_cast < std::size_t > (failure_cursor)] = mod_id;
        failure_cursor = (failure_cursor + 1) % cfg::THUMB_FAIL_RING;
      }
      [
        [nodiscard]
      ] bool is_in_flight(int mod_id) noexcept {
        return std::ranges::find(in_flight, mod_id) != in_flight.end();
      }
      [
        [nodiscard]
      ] TextureSlot * pick_slot() noexcept {
        for (TextureSlot & slot: slots) {
          if (slot.mod_id == 0) {
            return & slot;
          }
        }
        TextureSlot * best = nullptr;
        for (TextureSlot & slot: slots) {
          if (is_wanted(slot.mod_id)) {
            continue;
          }
          if (best == nullptr || slot.last_used < best -> last_used) {
            best = & slot;
          }
        }
        return best;
      }
    void rebuild_wanted() noexcept {
      wanted_count = 0;
      const int visible = model::visible_count();
      for (int i = 0; i < visible && wanted_count < cfg::CARDS_PER_PAGE; ++i) {
        const store::ModData & mod = model::mods[static_cast < std::size_t > (model::window_start + i)];
        if (mod.id != 0 && !mod.thumbnail_url.empty()) {
          wanted[static_cast < std::size_t > (wanted_count++)] = mod.id;
        }
      }
    }
    void rebuild_queue() noexcept {
      queue_size = 0;
      const int visible = model::visible_count();
      for (int i = 0; i < visible && queue_size < cfg::THUMB_QUEUE; ++i) {
        const store::ModData & mod = model::mods[static_cast < std::size_t > (model::window_start + i)];
        if (mod.id == 0 || mod.thumbnail_url.empty()) {
          continue;
        }
        if (mod.thumbnail_url.size() >= cfg::THUMB_URL_MAX) {
          continue;
        }
        if (is_resident(mod.id) || has_failed(mod.id) || is_in_flight(mod.id)) {
          continue;
        }
        Request & request = queue[static_cast < std::size_t > (queue_size++)];
        request.mod_id = mod.id;
        request.url.fill('\0');
        std::ranges::copy(mod.thumbnail_url, request.url.begin());
      }
    }
    void worker_main(void * argument) {
      const int index = static_cast < WorkerContext * > (argument) -> index;
      Publication & publication = publications[static_cast < std::size_t > (index)];
      sys::CurlHandle curl;
      if (curl) {
        configure_download(curl.get());
      }
      for (;;) {
        wake.clear();
        Request request;
        {
          const std::scoped_lock guard {
            lock
          };
          if (!quit_requested.load(std::memory_order_relaxed) && queue_size > 0) {
            request = queue[0];
            for (int i = 1; i < queue_size; ++i) {
              queue[static_cast < std::size_t > (i - 1)] =
                queue[static_cast < std::size_t > (i)];
            }
            --queue_size;
            in_flight[static_cast < std::size_t > (index)] = request.mod_id;
          }
        }
        if (quit_requested.load(std::memory_order_relaxed)) {
          break;
        }
        if (request.mod_id == 0) {
          wake.wait_for(cfg::THUMB_IDLE_WAIT_NS);
          continue;
        }
        const bool produced =
          produce(curl.get(), request.mod_id, request.url.data(),
            publication.pixels.as < u16 > ());
        if (quit_requested.load(std::memory_order_relaxed)) {
          break;
        }
        publication.ack.clear();
        {
          const std::scoped_lock guard {
            lock
          };
          publication.mod_id = request.mod_id;
          publication.ok = produced;
          publication.ready = true;
        }
        for (;;) {
          publication.ack.wait_for(cfg::THUMB_IDLE_WAIT_NS);
          bool taken = false;
          {
            const std::scoped_lock guard {
              lock
            };
            taken = !publication.ready;
          }
          if (taken || quit_requested.load(std::memory_order_relaxed)) {
            break;
          }
        }
        if (quit_requested.load(std::memory_order_relaxed)) {
          break;
        }
      }
      const std::scoped_lock guard {
        lock
      };
      in_flight[static_cast < std::size_t > (index)] = 0;
    }
    void tick() {
        if (!ready) {
          return;
        }
        ++frame_counter;
        bool have_work = false;
        {
          const std::scoped_lock guard {
            lock
          };
          rebuild_wanted();
          for (std::size_t i = 0; i < publications.size(); ++i) {
            Publication & publication = publications[i];
            if (!publication.ready) {
              continue;
            }
            if (!publication.ok) {
              mark_failed(publication.mod_id);
            } else if (is_wanted(publication.mod_id) && !is_resident(publication.mod_id)) {
              if (TextureSlot * slot = pick_slot()) {
                publication.pixels.swap_with(slot -> texture.data);
                slot -> mod_id = publication.mod_id;
                slot -> last_used = frame_counter;
              }
            }
            in_flight[i] = 0;
            publication.ready = false;
            publication.ack.signal();
          }
          rebuild_queue();
          have_work = queue_size > 0;
        }
        if (have_work) {
          wake.signal();
        }
      }
      [
        [nodiscard]
      ] C3D_Tex * texture_for(int mod_id) noexcept {
        if (!ready || mod_id == 0) {
          return nullptr;
        }
        for (TextureSlot & slot: slots) {
          if (slot.mod_id != mod_id) {
            continue;
          }
          slot.last_used = frame_counter;
          return & slot.texture;
        }
        return nullptr;
      }
    bool init() {
      if (ready) {
        return true;
      }
      (void) sd::make_directories(cfg::THUMB_DIR.view());
      build_sub_texture();
      std::size_t created = 0;
      for (; created < slots.size(); ++created) {
        TextureSlot & slot = slots[created];
        if (!C3D_TexInit( & slot.texture, cfg::THUMB_TEX_W, cfg::THUMB_TEX_H, GPU_RGB565)) {
          break;
        }
        C3D_TexSetFilter( & slot.texture, GPU_LINEAR, GPU_LINEAR);
        std::memset(slot.texture.data, 0, cfg::THUMB_TEX_BYTES);
        C3D_TexFlush( & slot.texture);
        slot.mod_id = 0;
        slot.last_used = 0;
      }
      std::size_t staged = 0;
      if (created == slots.size()) {
        for (; staged < publications.size(); ++staged) {
          Publication & publication = publications[staged];
          publication.pixels = sys::LinearBuffer {
            cfg::THUMB_TEX_BYTES
          };
          if (!publication.pixels) {
            break;
          }
          publication.mod_id = 0;
          publication.ready = false;
          publication.ok = false;
          in_flight[staged] = 0;
        }
      }
      if (created != slots.size() || staged != publications.size()) {
        for (std::size_t i = 0; i < created; ++i) {
          C3D_TexDelete( & slots[i].texture);
        }
        for (std::size_t i = 0; i < staged; ++i) {
          publications[i].pixels.reset();
        }
        for (TextureSlot & slot: slots) {
          slot.mod_id = 0;
        }
        return false;
      }
      quit_requested.store(false, std::memory_order_relaxed);
      ready = true;
      for (std::size_t i = 0; i < workers.size(); ++i) {
        worker_contexts[i].index = static_cast < int > (i);
        workers[i] = sys::Thread::spawn( & worker_main, & worker_contexts[i],
          cfg::WORKER_STACK_SIZE, cfg::WORKER_PRIORITY,
          cfg::ANY_CORE);
      }
      return true;
    }
    void shutdown() {
      if (!ready) {
        return;
      }
      ready = false;
      quit_requested.store(true, std::memory_order_relaxed);
      wake.signal();
      for (Publication & publication: publications) {
        publication.ack.signal();
      }
      for (sys::Thread & worker: workers) {
        worker.join();
      }
      for (Publication & publication: publications) {
        publication.pixels.reset();
      }
      for (TextureSlot & slot: slots) {
        C3D_TexDelete( & slot.texture);
        slot.mod_id = 0;
      }
    }
  }
  namespace draw {
    struct CardText {
      C2D_Text name {};
      C2D_Text author {};
      C2D_Text status {};
      bool has_name = false;
      bool has_author = false;
      bool has_status = false;
      float name_width = 0.0f;
      float author_width = 0.0f;
      float status_width = 0.0f;
    };
    C2D_TextBuf card_buffer = nullptr;
    C2D_TextBuf scratch_buffer = nullptr;
    std::array < CardText, cfg::CARDS_PER_PAGE > card_text;
    int card_text_count = 0;
    float font_line_height = 30.0f;
    float name_scale = 1.0f;
    float author_scale = 1.0f;
    float status_scale = 1.0f;
    float message_scale = 1.0f;
    [
      [nodiscard]
    ] float measure_c2d(const char * content, float scale) {
        if (scratch_buffer == nullptr || content == nullptr || content[0] == '\0') {
          return 0.0f;
        }
        C2D_TextBufClear(scratch_buffer);
        C2D_Text parsed;
        if (!C2D_TextFontParseLine( & parsed, nullptr, scratch_buffer, content, 0)) {
          return 0.0f;
        }
        return parsed.width * scale;
      }
      [
        [nodiscard]
      ] float measure_imgui(const char * content) {
        return ImGui::CalcTextSize(content).x;
      }
      [
        [nodiscard]
      ] constexpr u32 status_color(model::Priority priority) noexcept {
        switch (priority) {
        case model::Priority::UPDATE_AVAILABLE:
          return cfg::CLR_AMBER;
        case model::Priority::INSTALLED:
          return cfg::CLR_GREEN;
        case model::Priority::NOT_INSTALLED:
          break;
        }
        return cfg::CLR_GOLD;
      }
      [
        [nodiscard]
      ] bool init() {
        if (R_FAILED(fontEnsureMapped())) {
          return false;
        }
        const FINF_s * info = C2D_FontGetInfo(nullptr);
        if (info != nullptr && info -> lineFeed > 0) {
          font_line_height = static_cast < float > (info -> lineFeed);
        }
        name_scale = cfg::NAME_PX / font_line_height;
        author_scale = cfg::AUTHOR_PX / font_line_height;
        status_scale = cfg::STATUS_PX / font_line_height;
        message_scale = cfg::MSG_PX / font_line_height;
        card_buffer = C2D_TextBufNew(cfg::CARD_TEXT_GLYPHS);
        scratch_buffer = C2D_TextBufNew(cfg::SCRATCH_GLYPHS);
        return card_buffer != nullptr && scratch_buffer != nullptr;
      }
    void shutdown() noexcept {
      if (card_buffer != nullptr) {
        C2D_TextBufDelete(card_buffer);
        card_buffer = nullptr;
      }
      if (scratch_buffer != nullptr) {
        C2D_TextBufDelete(scratch_buffer);
        scratch_buffer = nullptr;
      }
      card_text_count = 0;
    }
    void rebuild_card_text() {
      model::cards_dirty = false;
      card_text_count = 0;
      if (card_buffer == nullptr) {
        return;
      }
      C2D_TextBufClear(card_buffer);
      const auto parse = [](std::string_view source, float scale, C2D_Text & out,
        float & out_width) -> bool {
        const std::string fitted = text::fit(
          source, cfg::TEXT_MAX_W, [scale](const char * candidate) {
            return measure_c2d(candidate, scale);
          });
        if (fitted.empty()) {
          return false;
        }
        if (!C2D_TextFontParseLine( & out, nullptr, card_buffer, fitted.c_str(), 0)) {
          return false;
        }
        C2D_TextOptimize( & out);
        out_width = out.width * scale;
        return true;
      };
      const int visible = model::visible_count();
      for (int i = 0; i < visible; ++i) {
        const store::ModData & mod = model::mods[static_cast < std::size_t > (model::window_start + i)];
        CardText & entry = card_text[static_cast < std::size_t > (i)];
        entry.has_name = parse(mod.name, name_scale, entry.name, entry.name_width);
        entry.has_author = parse(mod.author, author_scale, entry.author, entry.author_width);
        const model::Priority priority = model::priority_of(mod);
        entry.has_status =
          priority > model::Priority::NOT_INSTALLED &&
          parse(priority == model::Priority::UPDATE_AVAILABLE ? "Update Available" :
            "Installed",
            status_scale, entry.status, entry.status_width);
      }
      card_text_count = visible;
    }
    void draw_card(int slot,
      const store::ModData & mod, bool selected) {
      const float x = static_cast < float > (slot % cfg::GRID_COLS) * cfg::CELL_W + cfg::CARD_MARGIN;
      const float y = static_cast < float > (slot / cfg::GRID_COLS) * cfg::CELL_H + cfg::CARD_MARGIN;
      const model::Priority priority = model::priority_of(mod);
      const u32 accent = status_color(priority);
      C2D_DrawRectSolid(x, y, 0.0f, cfg::CARD_W, cfg::CARD_H,
        selected ? accent : cfg::CLR_BG);
      const float content_x = x + cfg::CARD_BORDER;
      const float content_y = y + cfg::CARD_BORDER;
      C2D_DrawRectSolid(content_x, content_y, 0.0f, cfg::CONTENT_W, cfg::CONTENT_H,
        selected ? cfg::CLR_SEL_BG : cfg::CLR_BG);
      if (C3D_Tex * texture = thumbs::texture_for(mod.id)) {
        const C2D_Image image {
          texture,
          & thumbs::sub_texture
        };
        C2D_DrawImageAt(image, content_x, content_y, 0.0f, nullptr, 1.0f, 1.0f);
      } else {
        C2D_DrawRectSolid(content_x, content_y, 0.0f, cfg::CONTENT_W, cfg::THUMB_H,
          cfg::CLR_THUMB);
      }
      if (slot >= card_text_count) {
        return;
      }
      const CardText & entry = card_text[static_cast < std::size_t > (slot)];
      if (entry.has_name) {
        C2D_DrawText( & entry.name, C2D_WithColor,
          content_x + (cfg::CONTENT_W - entry.name_width) * 0.5f,
          content_y + cfg::NAME_Y, 0.0f, name_scale, name_scale, accent);
      }
      if (entry.has_author) {
        C2D_DrawText( & entry.author, C2D_WithColor,
          content_x + (cfg::CONTENT_W - entry.author_width) * 0.5f,
          content_y + cfg::AUTHOR_Y, 0.0f, author_scale, author_scale,
          cfg::CLR_AUTHOR);
      }
      if (entry.has_status) {
        C2D_DrawText( & entry.status, C2D_WithColor,
          content_x + (cfg::CONTENT_W - entry.status_width) * 0.5f,
          content_y + cfg::STATUS_Y, 0.0f, status_scale, status_scale,
          accent);
      }
    }
    void draw_top_message(const char * message, u32 color) {
      if (scratch_buffer == nullptr || message == nullptr || message[0] == '\0') {
        return;
      }
      C2D_TextBufClear(scratch_buffer);
      C2D_Text parsed;
      if (!C2D_TextFontParseLine( & parsed, nullptr, scratch_buffer, message, 0)) {
        return;
      }
      C2D_TextOptimize( & parsed);
      C2D_DrawText( & parsed, C2D_WithColor,
        (cfg::TOP_W - parsed.width * message_scale) * 0.5f,
        (cfg::TOP_H - font_line_height * message_scale) * 0.5f,
        0.0f, message_scale, message_scale, color);
    }
    void top_screen() {
      if (model::state == model::AppState::FAILED) {
        draw_top_message(model::error_text.c_str(), cfg::CLR_ERROR);
        return;
      }
      if (model::state != model::AppState::BROWSING) {
        return;
      }
      if (model::cards_dirty) {
        rebuild_card_text();
      }
      const int visible = model::visible_count();
      if (visible <= 0) {
        draw_top_message("No mods found.", cfg::CLR_ERROR);
        return;
      }
      for (int i = 0; i < visible; ++i) {
        draw_card(i, model::mods[static_cast < std::size_t > (model::window_start + i)],
          i == model::selected);
      }
    }
    void text_centered(const char * content,
      const ImVec4 & color, float y) {
      ImGui::SetCursorPos(ImVec2((cfg::BOT_W - ImGui::CalcTextSize(content).x) * 0.5f, y));
      ImGui::TextColored(color, "%s", content);
    }
    void bottom_status(const std::string & content, bool failed) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(cfg::BOT_W, cfg::BOT_H));
        ImGui::Begin("##status", nullptr, cfg::SCREEN_WINDOW_FLAGS);
        const ImVec4 & color = failed ? cfg::IM_ERROR : cfg::IM_GOLD;
        const float wrap_width = cfg::BOT_W - 20.0f;
        const ImVec2 single = ImGui::CalcTextSize(content.c_str());
        if (single.x <= wrap_width) {
          text_centered(content.c_str(), color, (cfg::BOT_H - single.y) * 0.5f);
        } else {
          const ImVec2 wrapped =
            ImGui::CalcTextSize(content.c_str(), nullptr, false, wrap_width);
          ImGui::SetCursorPos(ImVec2(10.0f, (cfg::BOT_H - wrapped.y) * 0.5f));
          ImGui::PushTextWrapPos(10.0f + wrap_width);
          ImGui::PushStyleColor(ImGuiCol_Text, color);
          ImGui::TextWrapped("%s", content.c_str());
          ImGui::PopStyleColor();
          ImGui::PopTextWrapPos();
        }
        if (failed) {
          text_centered("Press START to exit.", cfg::IM_AUTHOR, cfg::BOT_H - 40.0f);
        }
        ImGui::End();
      }
      [
        [nodiscard]
      ] std::string action_label(model::ModAction action,
        const store::ModData * mod) {
        switch (action) {
        case model::ModAction::INSTALLED:
          return "Installed";
        case model::ModAction::NONE:
          return "No mods";
        case model::ModAction::INSTALL:
        case model::ModAction::UPDATE:
          break;
        }
        if (mod == nullptr) {
          return "No mods";
        }
        return fmt::format("{}{}", action == model::ModAction::UPDATE ? "Update " : "Install ",
          mod -> name);
      }
    void wrap_lines(std::string_view content, float wrap_width, int max_lines,
        std::vector < std::string > & out) {
        out.clear();
        if (content.empty() || wrap_width <= 0.0f || max_lines <= 0) {
          return;
        }
        ImFont * font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const char * cursor = content.data();
        const char * end = content.data() + content.size();
        while (cursor < end && static_cast < int > (out.size()) < max_lines) {
          const char * stop = font -> CalcWordWrapPosition(size, cursor, end, wrap_width);
          if (stop <= cursor) {
            stop = cursor + 1;
          }
          if (static_cast < int > (out.size()) == max_lines - 1 && stop < end) {
            const std::string_view remainder {
              cursor,
              static_cast < std::size_t > (end - cursor)
            };
            out.push_back(text::fit(remainder, wrap_width, & measure_imgui));
            return;
          }
          out.emplace_back(cursor, stop);
          cursor = stop;
          while (cursor < end && * cursor == ' ') {
            ++cursor;
          }
        }
      }
      [
        [nodiscard]
      ] bool wrapped_button(const char * id,
        const std::string & label, float y,
          float height,
          const ImVec4 & background,
            const ImVec4 & background_hot,
              const ImVec4 & foreground, float border) {
        ImGui::SetCursorPos(ImVec2(cfg::BTN_X, y));
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(cfg::BTN_W, height));
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImDrawList * list = ImGui::GetWindowDrawList();
        list -> AddRectFilled(top_left, bottom_right,
          ImGui::GetColorU32(hot ? background_hot : background),
          cfg::BTN_ROUNDING);
        if (border > 0.0f) {
          list -> AddRect(top_left, bottom_right, ImGui::GetColorU32(foreground),
            cfg::BTN_ROUNDING, 0, border);
        }
        std::vector < std::string > lines;
        wrap_lines(label, cfg::BTN_W - cfg::BTN_TEXT_PAD * 2.0f, cfg::BTN_MAX_LINES, lines);
        const float line_height = ImGui::GetTextLineHeight();
        const ImU32 color = ImGui::GetColorU32(foreground);
        float text_y = top_left.y + (height - line_height * static_cast < float > (lines.size())) * 0.5f;
        for (const std::string & line: lines) {
          const float width = ImGui::CalcTextSize(line.c_str()).x;
          list -> AddText(ImVec2(top_left.x + (cfg::BTN_W - width) * 0.5f, text_y), color,
            line.c_str());
          text_y += line_height;
        }
        return pressed;
      }
    void draw_sort_options() {
      text_centered("Sort Options", cfg::IM_GOLD, cfg::SORT_LABEL_Y);
      const ImGuiStyle & style = ImGui::GetStyle();
      const float radio = ImGui::GetFrameHeight();
      const float name_width = radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("By Name").x;
      const float updated_width =
        radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Recently Updated").x;
      ImGui::SetCursorPos(ImVec2(
        (cfg::BOT_W - (name_width + style.ItemSpacing.x + updated_width)) * 0.5f,
        cfg::SORT_ROW_Y));
      if (ImGui::RadioButton("By Name", model::sort_by_name)) {
        model::set_sort_mode(true);
      }
      ImGui::SameLine(0.0f, style.ItemSpacing.x);
      if (ImGui::RadioButton("Recently Updated", !model::sort_by_name)) {
        model::set_sort_mode(false);
      }
    }
    void draw_progress_bar() {
      ImDrawList * list = ImGui::GetWindowDrawList();
      list -> AddRectFilled(ImVec2(cfg::BTN_X, cfg::PROG_BAR_Y),
        ImVec2(cfg::BTN_X + cfg::BTN_W, cfg::PROG_BAR_Y + cfg::PROG_BAR_H),
        cfg::CLR_SEP);
      const int done = install::percent.load(std::memory_order_relaxed);
      if (done <= 0) {
        return;
      }
      const float filled =
        cfg::BTN_W * static_cast < float > (std::min(done, 100)) / 100.0f;
      list -> AddRectFilled(ImVec2(cfg::BTN_X, cfg::PROG_BAR_Y),
        ImVec2(cfg::BTN_X + filled, cfg::PROG_BAR_Y + cfg::PROG_BAR_H),
        ImGui::GetColorU32(cfg::IM_GOLD));
    }
    void bottom_browse() {
      ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
      ImGui::SetNextWindowSize(ImVec2(cfg::BOT_W, cfg::BOT_H));
      ImGui::Begin("##browse", nullptr, cfg::SCREEN_WINDOW_FLAGS);
      draw_sort_options();
      ImGui::GetWindowDrawList() -> AddRectFilled(
        ImVec2(cfg::BTN_X, cfg::SEP_Y),
        ImVec2(cfg::BTN_X + cfg::BTN_W, cfg::SEP_Y + 2.0f),
        cfg::CLR_SEP);
      const bool busy = install::busy();
      {
        const model::ModAction action = model::current_action();
        const std::string label =
          busy ? install::progress_label() : action_label(action, model::selected_mod());
        const ImVec4 & foreground =
          action == model::ModAction::UPDATE ? cfg::IM_AMBER : cfg::IM_GOLD;
        ImGui::BeginDisabled(busy || (action != model::ModAction::INSTALL &&
          action != model::ModAction::UPDATE));
        if (wrapped_button("##action", label, cfg::ACTION_BTN_Y, cfg::ACTION_BTN_H,
            cfg::IM_BTN_BG, cfg::IM_BTN_HOT, foreground, 2.0f)) {
          install::do_action();
        }
        ImGui::EndDisabled();
      }
      if (busy) {
        draw_progress_bar();
      } {
        const store::ModData * mod = model::selected_mod();
        const bool can_uninstall = !busy && mod != nullptr &&
          store::installed.contains(mod -> id);
        ImGui::BeginDisabled(!can_uninstall);
        if (wrapped_button("##uninstall", "Uninstall", cfg::UNINST_BTN_Y,
            cfg::UNINST_BTN_H, cfg::IM_UNINST_BG, cfg::IM_UNINST_HOT,
            cfg::IM_UNINST_FG, 1.0f)) {
          install::uninstall();
        }
        ImGui::EndDisabled();
      } {
        const int total = model::total_count();
        const int current =
          (total > 0 && model::selected_mod() != nullptr) ?
          model::window_start + model::selected + 1 :
          0;
        const std::string counter = fmt::format("{}/{}", current, total);
        text_centered(counter.c_str(), cfg::IM_AUTHOR, cfg::COUNTER_Y);
      }
      if (!install::user_message.empty()) {
        std::vector < std::string > lines;
        wrap_lines(install::user_message, cfg::BOT_W - 16.0f, cfg::MSG_MAX_LINES, lines);
        float y = cfg::MSG_LINE_Y;
        for (const std::string & line: lines) {
          text_centered(line.c_str(), cfg::IM_ERROR, y);
          y += ImGui::GetTextLineHeight() + 2.0f;
        }
      } else {
        text_centered("[A] Install  [B] Uninstall  [X] Sort", cfg::IM_AUTHOR, cfg::HINT1_Y);
        text_centered(busy ? "[B] Cancel  [START] Exit" : "[START] Exit",
          cfg::IM_AUTHOR, cfg::HINT2_Y);
      }
      ImGui::End();
    }
  }
  namespace app {
    [
      [nodiscard]
    ] bool enter_browse_state() {
      (void) store::load_installed();
      const char * preferred =
        model::sort_by_name ? cfg::BY_NAME_FILE.c_str() : cfg::BY_UPDATED_FILE.c_str();
      if (!store::read_mod_list(preferred, model::mods) || model::mods.empty()) {
        (void) store::read_mod_list(cfg::MOD_LIST_FILE.c_str(), model::mods);
      }
      if (model::mods.empty()) {
        return false;
      }
      model::window_start = 0;
      model::selected = 0;
      model::sort();
      (void) thumbs::init();
      (void) install::init();
      return true;
    }
    void advance_state(bool feed_done) {
      if (model::state == model::AppState::FETCHING && feed_done) {
        model::state = model::AppState::LOADING;
        status::print("Loading mods...");
        return;
      }
      if (model::state != model::AppState::LOADING) {
        return;
      }
      if (enter_browse_state()) {
        model::state = model::AppState::BROWSING;
        return;
      }
      model::state = model::AppState::FAILED;
      model::error_text = "Failed to load mods.";
    }
    void handle_browse_input(u32 nav_keys, u32 pressed) {
      model::handle_nav(nav_keys);
      if (nav_keys != 0) {
        install::user_message.clear();
      }
      if (install::busy()) {
        if (pressed & KEY_B) {
          install::cancel();
        }
        if (pressed & KEY_X) {
          model::set_sort_mode(!model::sort_by_name);
        }
        return;
      }
      if (pressed & KEY_A) {
        install::do_action();
      }
      if (pressed & KEY_B) {
        install::uninstall();
      }
      if (pressed & KEY_X) {
        model::set_sort_mode(!model::sort_by_name);
      }
    }
    class Platform {
      public: Platform() =
        default;
      Platform(const Platform & ) = delete;
      Platform & operator = (const Platform & ) = delete;
      ~Platform() {
        shutdown();
      }
      [
        [nodiscard]
      ] bool init() {
        gfxInitDefault();
        C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
        C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
        C2D_Prepare();
        graphics_ready_ = true;
        status::print("Starting up...");
        return init_screens() && init_romfs() && init_audio_and_network();
      }
      void run() {
        while (aptMainLoop()) {
          hidScanInput();
          const u32 pressed = hidKeysDown();
          const u32 held = hidKeysHeld();
          const u32 released = hidKeysUp();
          const float delta = tick_clock();
          update_pointer(held, released);
          const bool feed_done = status::feed_finished();
          advance_state(feed_done);
          const std::string status_line = status::current();
          if (feed_done && (pressed & KEY_START)) {
            break;
          }
          if (model::state == model::AppState::BROWSING) {
            install::tick();
            handle_browse_input(model::nav_repeat(pressed, held, delta), pressed);
          }
          render(status_line);
        }
      }
      private: [
          [nodiscard]
        ] bool init_screens() {
          top_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
          bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
          if (top_ == nullptr || bottom_ == nullptr) {
            status::print("Fatal: failed to create render targets");
            return false;
          }
          ImGui::CreateContext();
          imgui_ready_ = true;
          io_ = & ImGui::GetIO();
          io_ -> DisplaySize = ImVec2(cfg::BOT_W, cfg::BOT_H);
          imgui_sw::bind_imgui_painting(16.0f);
          imgui_sw::make_style_fast();
          ImGuiStyle & style = ImGui::GetStyle();
          style.Colors[ImGuiCol_WindowBg] = cfg::IM_WINDOW_BG;
          style.WindowRounding = 0.0f;
          style.WindowPadding = ImVec2(0.0f, 0.0f);
          style.WindowBorderSize = 0.0f;
          style.DisabledAlpha = 0.35f;
          io_ -> DeltaTime = 1.0f / 60.0f;
          osTickCounterStart( & frame_timer_);
          if (!draw::init()) {
            status::print("Fatal: failed to set up text rendering");
            return false;
          }
          return true;
        }
        [
          [nodiscard]
        ] bool init_romfs() {
          if (R_FAILED(romfsInit())) {
            status::print("romfsInit failed - no CA bundle!");
            return false;
          }
          romfs_ready_ = true;
          if (!sd::exists(cfg::CA_BUNDLE_PATH)) {
            status::print("Fatal: CA bundle missing from ROMFS");
            return false;
          }
          return true;
        }
        [
          [nodiscard]
        ] bool init_audio_and_network() {
          audio::init();
          soc_buffer_.reset(
            static_cast < u32 * > (memalign(cfg::SOC_ALIGN, cfg::SOC_BUFFERSIZE)));
          if (!soc_buffer_ ||
            R_FAILED(socInit(soc_buffer_.get(), static_cast < u32 > (cfg::SOC_BUFFERSIZE)))) {
            status::print("socInit failed!");
            return false;
          }
          soc_ready_ = true;
          if (R_FAILED(sslcInit(0))) {
            status::print("sslcInit failed!");
            return false;
          }
          sslc_ready_ = true;
          if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            status::print("curl_global_init failed!");
            return false;
          }
          curl_ready_ = true;
          feed_thread_ = sys::Thread::spawn( & feed::thread_main, nullptr, 128 * 1024,
            cfg::WORKER_PRIORITY, cfg::ANY_CORE);
          if (!feed_thread_) {
            status::print("Fatal: failed to create fetch thread");
            return false;
          }
          return true;
        }
        [
          [nodiscard]
        ] float tick_clock() noexcept {
          osTickCounterUpdate( & frame_timer_);
          float delta = static_cast < float > (osTickCounterRead( & frame_timer_) * 0.001);
          osTickCounterStart( & frame_timer_);
          if (!(delta > 0.0f)) {
            delta = 1.0f / 60.0f;
          }
          io_ -> DeltaTime = delta;
          return delta;
        }
      void update_pointer(u32 held, u32 released) noexcept {
        touchPosition touch {};
        hidTouchRead( & touch);
        if (held & KEY_TOUCH) {
          io_ -> MouseDown[0] = true;
          io_ -> MousePos = ImVec2(static_cast < float > (touch.px), static_cast < float > (touch.py));
          return;
        }
        io_ -> MouseDown[0] = false;
        if (!(released & KEY_TOUCH)) {
          io_ -> MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        }
      }
      void render(const std::string & status_line) {
        ImGui::NewFrame();
        if (model::state == model::AppState::BROWSING) {
          draw::bottom_browse();
        } else {
          const bool failed = model::state == model::AppState::FAILED;
          draw::bottom_status(failed ? model::error_text : status_line, failed);
        }
        ImGui::Render();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        thumbs::tick();
        C2D_TargetClear(top_, cfg::CLR_BG);
        C2D_SceneBegin(top_);
        C2D_Prepare();
        draw::top_screen();
        C2D_TargetClear(bottom_, cfg::CLR_BG);
        C2D_SceneBegin(bottom_);
        C2D_Prepare();
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
          GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
          GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        imgui_sw::paint_imgui(static_cast < int > (cfg::BOT_W), static_cast < int > (cfg::BOT_H),
          sw_options_);
        C2D_Flush();
        C3D_FrameEnd(0);
      }
      void shutdown() noexcept {
        feed_thread_.join();
        thumbs::shutdown();
        install::shutdown();
        if (curl_ready_) {
          curl_global_cleanup();
          curl_ready_ = false;
        }
        if (sslc_ready_) {
          sslcExit();
          sslc_ready_ = false;
        }
        if (soc_ready_) {
          socExit();
          soc_ready_ = false;
        }
        soc_buffer_.reset();
        audio::shutdown();
        if (romfs_ready_) {
          romfsExit();
          romfs_ready_ = false;
        }
        draw::shutdown();
        if (imgui_ready_) {
          imgui_sw::unbind_imgui_painting();
          ImGui::DestroyContext();
          imgui_ready_ = false;
        }
        if (graphics_ready_) {
          C2D_Fini();
          C3D_Fini();
          gfxExit();
          graphics_ready_ = false;
        }
      }
      C3D_RenderTarget * top_ = nullptr;
      C3D_RenderTarget * bottom_ = nullptr;
      ImGuiIO * io_ = nullptr;
      imgui_sw::SwOptions sw_options_ {};
      TickCounter frame_timer_ {};
      sys::MallocArray < u32 > soc_buffer_;
      sys::Thread feed_thread_;
      bool graphics_ready_ = false;
      bool imgui_ready_ = false;
      bool romfs_ready_ = false;
      bool soc_ready_ = false;
      bool sslc_ready_ = false;
      bool curl_ready_ = false;
    };
  }
}
int main() {
  app::Platform platform;
  if (!platform.init()) {
    return 1;
  }
  platform.run();
  return 0;
}