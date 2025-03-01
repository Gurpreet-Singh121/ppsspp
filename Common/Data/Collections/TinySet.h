#pragma once

#include <vector>

// Insert-only small-set implementation. Performs no allocation unless MaxFastSize is exceeded.
// Can also be used as a small vector, then use push_back (or push_in_place) instead of insert.
// Duplicates are thus allowed if you use that, but not if you exclusively use insert.
template <class T, int MaxFastSize>
struct TinySet {
	~TinySet() { delete slowLookup_; }
	inline void insert(const T &t) {
		// Fast linear scan.
		for (int i = 0; i < fastCount; i++) {
			if (fastLookup_[i] == t)
				return;  // We already have it.
		}
		// Fast insertion
		if (fastCount < MaxFastSize) {
			fastLookup_[fastCount++] = t;
			return;
		}
		// Fall back to slow path.
		insertSlow(t);
	}
	inline void push_back(const T &t) {
		if (fastCount < MaxFastSize) {
			fastLookup_[fastCount++] = t;
			return;
		}
		if (!slowLookup_) {
			slowLookup_ = new std::vector<T>();
		}
		slowLookup_->push_back(t);
	}
	inline T *add_back() {
		if (fastCount < MaxFastSize) {
			return &fastLookup_[fastCount++];
		}
		if (!slowLookup_) {
			slowLookup_ = new std::vector<T>();
		}
		T t;
		slowLookup_->push_back(t);
		return slowLookup_->back();
	}
	void append(const TinySet<T, MaxFastSize> &other) {
		size_t otherSize = other.size();
		if (size() + otherSize <= MaxFastSize) {
			// Fast case
			for (int i = 0; i < otherSize; i++) {
				fastLookup_[fastCount + i] = other.fastLookup_[i];
			}
			fastCount += other.fastCount;
		} else {
			for (int i = 0; i < otherSize; i++) {
				push_back(other[i]);
			}
		}
	}
	bool contains(T t) const {
		for (int i = 0; i < fastCount; i++) {
			if (fastLookup_[i] == t)
				return true;
		}
		if (slowLookup_) {
			for (auto x : *slowLookup_) {
				if (x == t)
					return true;
			}
		}
		return false;
	}
	bool contains(const TinySet<T, MaxFastSize> &otherSet) {
		// Awkward, kind of ruins the fun.
		for (int i = 0; i < fastCount; i++) {
			if (otherSet.contains(fastLookup_[i]))
				return true;
		}
		if (slowLookup_) {
			for (auto x : *slowLookup_) {
				if (otherSet.contains(x))
					return true;
			}
		}
		return false;
	}
	void clear() {
		delete slowLookup_;
		slowLookup_ = nullptr;
		fastCount = 0;
	}
	bool empty() const {
		return fastCount == 0;
	}
	size_t size() const {
		if (!slowLookup_) {
			return fastCount;
		} else {
			return slowLookup_->size() + MaxFastSize;
		}
	}
	T &operator[] (size_t index) {
		if (index < MaxFastSize) {
			return fastLookup_[index];
		} else {
			return (*slowLookup_)[index - MaxFastSize];
		}
	}
	const T &operator[] (size_t index) const {
		if (index < MaxFastSize) {
			return fastLookup_[index];
		} else {
			return (*slowLookup_)[index - MaxFastSize];
		}
	}
	const T &back() const {
		return (*this)[size() - 1];
	}

private:
	void insertSlow(T t) {
		if (!slowLookup_) {
			slowLookup_ = new std::vector<T>();
		} else {
			for (size_t i = 0; i < slowLookup_->size(); i++) {
				if ((*slowLookup_)[i] == t)
					return;
			}
		}
		slowLookup_->push_back(t);
	}
	T fastLookup_[MaxFastSize];
	int fastCount = 0;
	std::vector<T> *slowLookup_ = nullptr;
};
