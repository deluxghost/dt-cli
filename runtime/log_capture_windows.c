#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log_capture.h"

#define RAW_QUEUE_BYTES (512u * 1024u)
#define RAW_RECORD_BYTES (64u * 1024u)
#define SEARCH_BACK_BYTES 128u
#define SEARCH_FORWARD_BYTES 96u

typedef void (*buffer_write_fn)(void *destination, const void *source, uint32_t length);

static SRWLOCK queue_lock = SRWLOCK_INIT;
static uint8_t raw_queue[RAW_QUEUE_BYTES];
static uint32_t queue_read;
static uint32_t queue_write;
static uint32_t queue_used;
static volatile LONG64 dropped_bytes;
static buffer_write_fn original_buffer_write;
static volatile LONG capture_enabled;
static volatile LONG start_state;
static char start_error[512];

static void set_error(const char *message)
{
    snprintf(start_error, sizeof(start_error), "%s", message);
}

static void copy_error(char *buffer, int buffer_size)
{
    if (buffer == NULL || buffer_size <= 0) {
        return;
    }

    snprintf(buffer, (size_t)buffer_size, "%s", start_error);
}

static int is_readable_section(const IMAGE_SECTION_HEADER *section)
{
    return (section->Characteristics & IMAGE_SCN_MEM_READ) != 0;
}

static int is_executable_section(const IMAGE_SECTION_HEADER *section)
{
    return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
}

static int is_executable_address(
    uint8_t *image_base,
    IMAGE_NT_HEADERS64 *nt_headers,
    const void *address)
{
    IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt_headers);
    uintptr_t target = (uintptr_t)address;

    for (unsigned int index = 0; index < nt_headers->FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *section = &sections[index];
        uintptr_t begin = (uintptr_t)(image_base + section->VirtualAddress);
        uintptr_t end = begin + section->Misc.VirtualSize;
        if (is_executable_section(section) && target >= begin && target < end) {
            return 1;
        }
    }

    return 0;
}

static void queue_copy_in(const uint8_t *source, uint32_t length)
{
    uint32_t first = RAW_QUEUE_BYTES - queue_write;
    if (first > length) {
        first = length;
    }

    memcpy(raw_queue + queue_write, source, first);
    memcpy(raw_queue, source + first, length - first);
    queue_write = (queue_write + length) % RAW_QUEUE_BYTES;
    queue_used += length;
}

static void queue_copy_out(uint8_t *destination, uint32_t length)
{
    uint32_t first = RAW_QUEUE_BYTES - queue_read;
    if (first > length) {
        first = length;
    }

    memcpy(destination, raw_queue + queue_read, first);
    memcpy(destination + first, raw_queue, length - first);
    queue_read = (queue_read + length) % RAW_QUEUE_BYTES;
    queue_used -= length;
}

static void capture_bytes(const void *source, uint32_t length)
{
    uint32_t record_count;
    uint64_t required_size;

    if (source == NULL || length == 0 || InterlockedCompareExchange(&capture_enabled, 0, 0) == 0) {
        return;
    }

    record_count = (length - 1) / RAW_RECORD_BYTES + 1;
    required_size = (uint64_t)length + (uint64_t)record_count * sizeof(uint32_t);
    if (required_size > RAW_QUEUE_BYTES) {
        InterlockedAdd64(&dropped_bytes, length);
        return;
    }
    if (!TryAcquireSRWLockExclusive(&queue_lock)) {
        InterlockedAdd64(&dropped_bytes, length);
        return;
    }

    if (required_size > RAW_QUEUE_BYTES - queue_used) {
        ReleaseSRWLockExclusive(&queue_lock);
        InterlockedAdd64(&dropped_bytes, length);
        return;
    }

    const uint8_t *cursor = (const uint8_t *)source;
    uint32_t remaining = length;
    while (remaining > 0) {
        uint32_t record_length = remaining > RAW_RECORD_BYTES ? RAW_RECORD_BYTES : remaining;
        queue_copy_in((const uint8_t *)&record_length, (uint32_t)sizeof(record_length));
        queue_copy_in(cursor, record_length);
        cursor += record_length;
        remaining -= record_length;
    }

    ReleaseSRWLockExclusive(&queue_lock);
}

static void capture_buffer_write(void *destination, const void *source, uint32_t length)
{
    original_buffer_write(destination, source, length);
    capture_bytes(source, length);
}

static uint8_t *find_unique_string(
    uint8_t *image_base,
    IMAGE_NT_HEADERS64 *nt_headers,
    const char *value)
{
    IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt_headers);
    size_t value_size = strlen(value) + 1;
    uint8_t *match = NULL;
    unsigned int count = 0;

    for (unsigned int index = 0; index < nt_headers->FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *section = &sections[index];
        if (!is_readable_section(section)) {
            continue;
        }

        uint8_t *begin = image_base + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        if (size < value_size) {
            continue;
        }

        for (size_t offset = 0; offset <= size - value_size; ++offset) {
            if (memcmp(begin + offset, value, value_size) == 0) {
                match = begin + offset;
                ++count;
            }
        }
    }

    return count == 1 ? match : NULL;
}

static uint8_t *find_unique_string_xref(
    uint8_t *image_base,
    IMAGE_NT_HEADERS64 *nt_headers,
    uint8_t *string_address)
{
    IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt_headers);
    uint8_t *match = NULL;
    unsigned int count = 0;

    for (unsigned int index = 0; index < nt_headers->FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *section = &sections[index];
        if (!is_executable_section(section)) {
            continue;
        }

        uint8_t *begin = image_base + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        for (size_t offset = 0; offset + 7 <= size; ++offset) {
            uint8_t *instruction = begin + offset;
            if (instruction[0] != 0x48 || instruction[1] != 0x8d ||
                (instruction[2] & 0xc7) != 0x05) {
                continue;
            }

            int32_t displacement;
            memcpy(&displacement, instruction + 3, sizeof(displacement));
            if (instruction + 7 + displacement == string_address) {
                match = instruction;
                ++count;
            }
        }
    }

    return count == 1 ? match : NULL;
}

static int validate_capture_sequence(uint8_t *candidate, uint8_t **call_site)
{
    if (candidate[0] != 0x44 || candidate[1] != 0x8b ||
        candidate[2] != 0x43 || candidate[4] != 0x48 ||
        candidate[5] != 0x8d || candidate[6] != 0x53 ||
        candidate[8] != 0x48 || candidate[9] != 0x8d ||
        candidate[10] != 0x4f || candidate[12] != 0xe8 ||
        candidate[17] != 0x80 || candidate[18] != 0xbf ||
        candidate[19] != 0x98 || candidate[20] != 0x00 ||
        candidate[21] != 0x00 || candidate[22] != 0x00 ||
        candidate[23] != 0x00) {
        return 0;
    }

    if (candidate[3] != 0x08 || candidate[7] != 0x10 || candidate[11] != 0x78) {
        return 0;
    }

    *call_site = candidate + 12;
    return 1;
}

static uint8_t *find_unique_call_site(uint8_t *xref)
{
    uint8_t *function_start = NULL;
    unsigned int starts = 0;

    for (uint32_t back = 0; back <= SEARCH_BACK_BYTES; ++back) {
        uint8_t *candidate = xref - back;
        if (candidate[0] == 0x40 && candidate[1] == 0x53 &&
            candidate[2] == 0x48 && candidate[3] == 0x83 &&
            candidate[4] == 0xec && candidate[5] == 0x20) {
            function_start = candidate;
            ++starts;
        }
    }
    if (starts != 1 || xref < function_start || xref - function_start > SEARCH_BACK_BYTES) {
        return NULL;
    }

    uint8_t *call_site = NULL;
    unsigned int calls = 0;
    for (uint32_t offset = 0; offset + 24 <= SEARCH_FORWARD_BYTES; ++offset) {
        uint8_t *candidate = xref + offset;
        uint8_t *candidate_call = NULL;
        if (validate_capture_sequence(candidate, &candidate_call)) {
            call_site = candidate_call;
            ++calls;
        }
    }

    return calls == 1 ? call_site : NULL;
}

static void *allocate_near(uint8_t *target)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t granularity = info.dwAllocationGranularity;
    uintptr_t target_value = (uintptr_t)target;
    uintptr_t lower = target_value > 0x7fff0000ULL ? target_value - 0x7fff0000ULL : 0;
    uintptr_t upper = target_value + 0x7fff0000ULL;

    lower = (lower + granularity - 1) & ~(granularity - 1);
    upper &= ~(granularity - 1);

    for (uintptr_t distance = 0; distance <= 0x7fff0000ULL; distance += granularity) {
        uintptr_t candidates[2] = {target_value + distance, target_value - distance};
        for (unsigned int index = 0; index < 2; ++index) {
            uintptr_t address = candidates[index] & ~(granularity - 1);
            if (address < lower || address > upper) {
                continue;
            }

            void *allocation = VirtualAlloc(
                (void *)address,
                granularity,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_EXECUTE_READWRITE);
            if (allocation != NULL) {
                return allocation;
            }
        }
    }

    return NULL;
}

static int patch_relative_call(uint8_t *call_site, int32_t displacement)
{
    uintptr_t word_address = (uintptr_t)call_site & ~(uintptr_t)7;
    size_t call_offset = (size_t)((uintptr_t)call_site - word_address);
    if (call_offset + 5 > sizeof(LONG64)) {
        set_error("the FileStream call site is not contained in one aligned 8-byte word");
        return 0;
    }

    uint8_t expected_bytes[sizeof(LONG64)];
    uint8_t patched_bytes[sizeof(LONG64)];
    memcpy(expected_bytes, (const void *)word_address, sizeof(expected_bytes));
    memcpy(patched_bytes, expected_bytes, sizeof(patched_bytes));
    if (expected_bytes[call_offset] != 0xe8) {
        set_error("the FileStream call opcode changed before hook installation");
        return 0;
    }
    memcpy(patched_bytes + call_offset + 1, &displacement, sizeof(displacement));

    LONG64 expected;
    LONG64 patched;
    memcpy(&expected, expected_bytes, sizeof(expected));
    memcpy(&patched, patched_bytes, sizeof(patched));

    DWORD old_protect;
    if (!VirtualProtect((void *)word_address, sizeof(LONG64), PAGE_EXECUTE_READWRITE, &old_protect)) {
        set_error("could not make the FileStream call site writable");
        return 0;
    }

    LONG64 observed = InterlockedCompareExchange64(
        (volatile LONG64 *)word_address,
        patched,
        expected);
    BOOL flushed = observed == expected
        ? FlushInstructionCache(GetCurrentProcess(), (const void *)word_address, sizeof(LONG64))
        : TRUE;

    DWORD ignored;
    BOOL restored = VirtualProtect((void *)word_address, sizeof(LONG64), old_protect, &ignored);
    if (observed != expected) {
        set_error("the FileStream call site changed before hook installation");
        return 0;
    }
    if (!flushed) {
        set_error("could not flush the patched FileStream call site");
        return 0;
    }
    if (!restored) {
        set_error("could not restore the FileStream call site protection");
        return 0;
    }

    return 1;
}

static int install_capture_hook(void)
{
    HMODULE pinned_module;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)(uintptr_t)&capture_buffer_write,
            &pinned_module)) {
        set_error("failed to pin the native logging runtime module");
        return 0;
    }

    uint8_t *image_base = (uint8_t *)GetModuleHandleW(NULL);
    if (image_base == NULL) {
        set_error("failed to locate the game executable module");
        return 0;
    }

    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)image_base;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        set_error("the game executable has an invalid DOS header");
        return 0;
    }

    IMAGE_NT_HEADERS64 *nt_headers = (IMAGE_NT_HEADERS64 *)(image_base + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
        nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        set_error("the game executable has an invalid PE header");
        return 0;
    }

    uint8_t *string_address = find_unique_string(
        image_base,
        nt_headers,
        "stingray::FileStream::flush_buffer");
    if (string_address == NULL) {
        set_error("could not uniquely locate the FileStream flush marker");
        return 0;
    }

    uint8_t *xref = find_unique_string_xref(image_base, nt_headers, string_address);
    if (xref == NULL) {
        set_error("could not uniquely locate the FileStream flush marker reference");
        return 0;
    }

    uint8_t *call_site = find_unique_call_site(xref);
    if (call_site == NULL) {
        set_error("could not uniquely validate the FileStream console write call");
        return 0;
    }

    int32_t original_displacement;
    memcpy(&original_displacement, call_site + 1, sizeof(original_displacement));
    original_buffer_write = (buffer_write_fn)(call_site + 5 + original_displacement);
    if (!is_executable_address(image_base, nt_headers, (const void *)original_buffer_write)) {
        set_error("the FileStream console write target is not executable");
        return 0;
    }

    uint8_t *relay = (uint8_t *)allocate_near(call_site);
    if (relay == NULL) {
        set_error("could not allocate a log hook relay near the game call site");
        return 0;
    }

    relay[0] = 0x48;
    relay[1] = 0xb8;
    void *wrapper = (void *)&capture_buffer_write;
    memcpy(relay + 2, &wrapper, sizeof(wrapper));
    relay[10] = 0xff;
    relay[11] = 0xe0;
    if (!FlushInstructionCache(GetCurrentProcess(), relay, 12)) {
        set_error("could not flush the log hook relay");
        return 0;
    }

    DWORD relay_old_protect;
    if (!VirtualProtect(relay, 12, PAGE_EXECUTE_READ, &relay_old_protect)) {
        set_error("could not protect the log hook relay");
        return 0;
    }

    intptr_t relay_displacement = relay - (call_site + 5);
    if (relay_displacement < INT32_MIN || relay_displacement > INT32_MAX) {
        set_error("the log hook relay is outside rel32 range");
        return 0;
    }

    int32_t patched_displacement = (int32_t)relay_displacement;
    if (!patch_relative_call(call_site, patched_displacement)) {
        return 0;
    }

    InterlockedExchange(&capture_enabled, 1);
    return 1;
}

int LuaExecLogCapture_Start(char *error_buffer, int error_buffer_size)
{
    LONG state = InterlockedCompareExchange(&start_state, 1, 0);
    if (state == 2) {
        return 1;
    }
    if (state == 3) {
        copy_error(error_buffer, error_buffer_size);
        return 0;
    }
    if (state == 1) {
        while ((state = InterlockedCompareExchange(&start_state, 0, 0)) == 1) {
            Sleep(1);
        }
        if (state == 2) {
            return 1;
        }
        copy_error(error_buffer, error_buffer_size);
        return 0;
    }

    if (!install_capture_hook()) {
        InterlockedExchange(&start_state, 3);
        copy_error(error_buffer, error_buffer_size);
        return 0;
    }

    InterlockedExchange(&start_state, 2);
    return 1;
}

int LuaExecLogCapture_Pop(
    char *buffer,
    uint32_t buffer_size,
    uint32_t *length,
    uint64_t *dropped)
{
    uint32_t record_length;

    if (buffer == NULL || length == NULL || dropped == NULL) {
        return -1;
    }

    AcquireSRWLockExclusive(&queue_lock);
    *dropped = (uint64_t)InterlockedExchange64(&dropped_bytes, 0);
    if (queue_used < sizeof(record_length)) {
        *length = 0;
        ReleaseSRWLockExclusive(&queue_lock);
        return 0;
    }

    queue_copy_out((uint8_t *)&record_length, (uint32_t)sizeof(record_length));
    if (record_length > buffer_size || record_length > queue_used) {
        *length = 0;
        ReleaseSRWLockExclusive(&queue_lock);
        return -1;
    }

    queue_copy_out((uint8_t *)buffer, record_length);
    *length = record_length;
    ReleaseSRWLockExclusive(&queue_lock);
    return 1;
}
