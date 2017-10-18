/* Tippse - Misc - Functions that fit nowhere at the moment */

#include "misc.h"

// Sort an array with merge sort, second array is a temporary buffer
char** merge_sort(char** sort1, char** sort2, size_t count) {
  size_t m;
  for (m = 1; m<count; m<<=1) {
    size_t n;
    for (n = 0; n<count; n+=m*2) {
      size_t left = n;
      size_t left_end = n+m;
      if (left_end>count) {
        left_end = count;
      }

      size_t right = n+m;
      size_t right_end = n+m*2;
      if (right_end>count) {
        right_end = count;
      }

      size_t out = n;
      while (left<left_end && right<right_end) {
        if (strcasecmp(sort1[left], sort1[right])<=0) {
          sort2[out++] = sort1[left];
          left++;
        } else {
          sort2[out++] = sort1[right];
          right++;
        }
      }

      while (left<left_end) {
        sort2[out++] = sort1[left];
        left++;
      }

      while (right<right_end) {
        sort2[out++] = sort1[right];
        right++;
      }
    }

    char** sort = sort2;
    sort2 = sort1;
    sort1 = sort;
  }

  return sort1;
}

// Return directory path without file name
char* strip_file_name(const char* file) {
  const char* last = file;
  const char* search = file;
  while (*search) {
    if (*search=='/') {
      last = search;
    }

    search++;
  }

  char* stripped = malloc(sizeof(char)*(size_t)((last-file)+1));
  memcpy(stripped, file, (size_t)(last-file));
  stripped[last-file] = '\0';

  return stripped;
}

// Return merged strings
char* combine_string(const char* string1, const char* string2) {
  size_t string1_length = strlen(string1);
  size_t string2_length = strlen(string2);
  char* combined = malloc(sizeof(char)*(string1_length+string2_length+1));
  memcpy(combined, string1, string1_length);
  memcpy(combined+string1_length, string2, string2_length);
  combined[string1_length+string2_length] = '\0';

  return combined;
}

// Returned merged directory path and file name
char* combine_path_file(const char* path, const char* file) {
  if (*file=='/') {
    return strdup(file);
  }

  size_t path_length = strlen(path);
  if (path_length==0) {
    return strdup(file);
  }

  if (path_length>0 && path[path_length-1]=='/') {
    path_length--;
  }

  size_t file_length = strlen(file);
  char* combined = malloc(sizeof(char)*(path_length+file_length+2));
  memcpy(combined, path, path_length);
  combined[path_length] = '/';
  memcpy(combined+path_length+1, file, file_length);
  combined[path_length+file_length+1] = '\0';

  return combined;
}

// Return normalised directory path
char* correct_path(const char* path) {
  size_t path_length = strlen(path);
  char* real = malloc(sizeof(char)*(path_length+2));
  char* combined = real;

  int directories = 0;
  while (*path) {
    if (path[0]=='/') {
      combined = real;
      directories = 0;

      *combined++ = '/';
      path++;
    } else if (path[0]=='.' && (path[1]=='/' || path[1]==0)) {
      path+=(path[1]==0)?1:2;
    } else if (directories>0 && path[0]=='.' && path[1]=='.' && (path[2]=='/' || path[2]==0)) {
      combined--;
      while (combined!=real && *(combined-1)!='/') {
        combined--;
      }
      directories--;
      path+=(path[2]==0)?2:3;
    } else if (combined!=real && *real=='/' && path[0]=='.' && path[1]=='.' && (path[2]=='/' || path[2]==0)) {
      path+=(path[2]==0)?2:3;
    } else {
      while (*path && *path!='/') {
        *combined++ = *path++;
      }

      if (*path=='/') {
        *combined++ = '/';
        directories++;
        path++;
      }
    }
  }

  if (combined!=real && combined!=real+1) {
    if (*(combined-1)=='/') {
      combined--;
    }
  }

  if (combined==real) {
    *combined++ = '.';
  }

  *combined = '\0';

  return real;
}

// Return relative directory path
char* relativate_path(const char* base, const char* path) {
  char* real = realpath(path, NULL);
  if (!real) {
    return strdup(path);
  }

  const char* search = real;
  while (*search==*base && *base!=0) {
    search++;
    base++;
  }

  if (*base==0) {
    if (*search=='/') {
      char* relative = strdup(search+1);
      free(real);
      return relative;
    } else if (*search==0) {
      char* relative = strdup(".");
      free(real);
      return relative;
    }
  }

  return real;
}

// Return user's home directory
char* home_path(void) {
  char* env = getenv("HOME");
  if (env) {
    return strdup(env);
  }

  struct passwd* pw = getpwuid(getuid());
  if (pw->pw_dir) {
    return strdup(pw->pw_dir);
  }

  return strdup("");
}

// Check if directory
int is_directory(const char* path) {
  struct stat statbuf;
  if (stat(path, &statbuf)!=0) {
    return 0;
  }

  return S_ISDIR(statbuf.st_mode);
}

// Return tick counter (microseconds)
int64_t tick_count(void) {
  struct timeval t;
  gettimeofday(&t, NULL);
  return (int64_t)t.tv_sec*1000000+(int64_t)t.tv_usec;
}
