// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SDL host save backend.  File-backed single slot.

#include "save_backend.h"
#include "common/stream.h"

#include <cstdio>
#include <cstring>

namespace tsb {
namespace save_backend {

namespace {

// Save file lives next to the game data files in the cwd.  No metadata
// or thumbnail — just the raw saveState() bytes.
const char *kSavePath = "thumbyscummby.sav";

// FILE* writer adapter.
class FileWriteStream : public Common::SeekableWriteStream {
public:
	explicit FileWriteStream(FILE *f) : _f(f), _err(false) {}
	~FileWriteStream() override {
		if (_f) {
			fflush(_f);
			fclose(_f);
		}
	}

	uint32 write(const void *dataPtr, uint32 dataSize) override {
		if (!_f) return 0;
		size_t n = fwrite(dataPtr, 1, dataSize, _f);
		if (n != dataSize) _err = true;
		return (uint32)n;
	}

	bool flush() override {
		if (!_f) return false;
		return fflush(_f) == 0;
	}

	void finalize() override {
		flush();
		if (_f) {
			fclose(_f);
			_f = nullptr;
		}
	}

	int64 pos() const override {
		return _f ? (int64)ftell(_f) : -1;
	}

	bool seek(int64 offset, int whence) override {
		if (!_f) return false;
		return fseek(_f, (long)offset, whence) == 0;
	}

	int64 size() const override {
		if (!_f) return -1;
		long cur = ftell(_f);
		fseek(_f, 0, SEEK_END);
		long sz = ftell(_f);
		fseek(_f, cur, SEEK_SET);
		return sz;
	}

	bool err() const override { return _err; }
	void clearErr() override { _err = false; if (_f) clearerr(_f); }

private:
	FILE *_f;
	bool _err;
};

// FILE* reader adapter.
class FileReadStream : public Common::SeekableReadStream {
public:
	explicit FileReadStream(FILE *f) : _f(f), _eos(false), _err(false) {}
	~FileReadStream() override {
		if (_f) fclose(_f);
	}

	uint32 read(void *dataPtr, uint32 dataSize) override {
		if (!_f) { _err = true; return 0; }
		size_t n = fread(dataPtr, 1, dataSize, _f);
		if (n < dataSize) {
			if (feof(_f)) _eos = true;
			else _err = true;
		}
		return (uint32)n;
	}

	bool eos() const override { return _eos; }
	bool err() const override { return _err; }
	void clearErr() override { _err = _eos = false; if (_f) clearerr(_f); }

	int64 pos() const override {
		return _f ? (int64)ftell(_f) : -1;
	}

	bool seek(int64 offset, int whence) override {
		if (!_f) return false;
		bool ok = (fseek(_f, (long)offset, whence) == 0);
		if (ok) _eos = false;
		return ok;
	}

	int64 size() const override {
		if (!_f) return -1;
		long cur = ftell(_f);
		fseek(_f, 0, SEEK_END);
		long sz = ftell(_f);
		fseek(_f, cur, SEEK_SET);
		return sz;
	}

private:
	FILE *_f;
	bool _eos;
	bool _err;
};

}  // anonymous

Common::SeekableWriteStream *open_for_writing() {
	FILE *f = fopen(kSavePath, "wb");
	if (!f) return nullptr;
	return new FileWriteStream(f);
}

Common::SeekableReadStream *open_for_reading() {
	FILE *f = fopen(kSavePath, "rb");
	if (!f) return nullptr;
	return new FileReadStream(f);
}

bool has_save() {
	FILE *f = fopen(kSavePath, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

}  // namespace save_backend
}  // namespace tsb
