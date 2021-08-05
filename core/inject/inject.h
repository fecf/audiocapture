#pragma once

struct Header {
  int header_offset;
  int header_size;
  int data_offset;
  int data_size;
  int total_size;

  int channels;
  int samples;
  int bits_per_sample;
  int sampling_rate;
} header;
