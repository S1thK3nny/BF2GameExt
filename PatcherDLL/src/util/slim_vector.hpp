#pragma once

#include <stdlib.h>

#include <initializer_list>

template<typename T>
struct slim_vector {
   struct alloc_tag {};

   slim_vector() = default;

   slim_vector(size_t size, alloc_tag)
   {
      _size = size;
      _data = new T[_size];

      if (not _data) abort();
   }

   slim_vector(std::initializer_list<T> objects) : slim_vector{objects.size(), alloc_tag{}}
   {
      for (size_t i = 0; i < _size; ++i) _data[i] = *(objects.begin() + i);
   }

   ~slim_vector()
   {
      if (_data) delete[] _data;
   }

   slim_vector(const slim_vector& other)
   {
      _size = other._size;
      _data = new T[_size];

      if (not _data) abort();

      for (size_t i = 0; i < _size; ++i) _data[i] = *(other._data + i);
   }

   auto operator=(const slim_vector& other) -> slim_vector&
   {
      if (_data) delete[] _data;

      _size = other._size;
      _data = new T[_size];

      if (not _data) abort();

      for (size_t i = 0; i < _size; ++i) _data[i] = *(other._data + i);

      return *this;
   }

   [[nodiscard]] auto data() const noexcept -> const T*
   {
      return _data;
   }

   [[nodiscard]] auto size() const noexcept -> size_t
   {
      return _size;
   }

   [[nodiscard]] auto operator[](size_t i) noexcept -> T&
   {
      if (not _data or i >= _size) abort();

      return _data[i];
   }

   [[nodiscard]] auto operator[](size_t i) const noexcept -> const T&
   {
      if (not _data or i >= _size) abort();

      return _data[i];
   }

   [[nodiscard]] auto begin() const noexcept -> const T*
   {
      return _data;
   }

   [[nodiscard]] auto end() const noexcept -> const T*
   {
      return _data + _size;
   }

   bool operator==(const slim_vector& other) const noexcept
   {

      if (this->_size != other._size) return false;

      for (size_t i = 0; i < other._size; ++i) {
         if (this->_data[i] != other._data[i]) return false;
      }

      return true;
   }

private:
   T* _data = nullptr;
   size_t _size = 0;
};
