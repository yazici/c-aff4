/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License.  You may obtain a copy of the
License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.  See the License for the
specific language governing permissions and limitations under the License.
*/

#include "aff4/aff4_base.h"
#include "aff4/aff4_file.h"
#include "aff4/aff4_directory.h"
#include "aff4/libaff4.h"
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif


namespace aff4 {


AFF4Status CreateIntermediateDirectories(
    DataStore *resolver, std::vector<std::string> components);

/***************************************************************
FileBackedObject implementation.
****************************************************************/
 AFF4Status NewFileBackedObject(
     DataStore *resolver,
     std::string filename,
     std::string mode,
     AFF4Flusher<AFF4Stream> &result
 ) {
     AFF4Flusher<FileBackedObject> file;
     RETURN_IF_ERROR(NewFileBackedObject(resolver, filename, mode, file));

     result.reset(file.release());

     return STATUS_OK;
 }

// Recursively create intermediate directories.
AFF4Status CreateIntermediateDirectories(
    DataStore *resolver, std::vector<std::string> components) {
    std::string path = PATH_SEP_STR;

#ifdef _WIN32
    // On windows leading \\ means a device - we do not want to create
    // any intermediate directories for devices.
    if (components.size() > 2 &&
        components[0] == "" && components[1] == "") {
        return STATUS_OK;
    }

    // On windows we do not want a leading \ (e.g. C:\windows not \C:\Windows)
    path = "";
#endif

    for (auto component : components) {
        path = path + component + PATH_SEP_STR;
        resolver->logger->debug("Creating intermediate directories {}",
                                path);

        if (AFF4Directory::IsDirectory(path, /* must_exist= */ true)) {
            continue;
        }

        // Directory does not exist - Try to make it.
        if (AFF4Directory::MkDir(resolver, path) == STATUS_OK) {
            continue;
        }

        resolver->logger->error(
            "Unable to create intermediate directory: {}",
            GetLastErrorMessage());
        return IO_ERROR;
    }

    return STATUS_OK;
}

AFF4Status CreateIntermediateDirectories(DataStore *resolver, std::string dir_name) {
    return CreateIntermediateDirectories(resolver, break_path_into_components(dir_name));
}

// Windows files are read through the CreateFile() API so that devices can be
// read.
#if defined(_WIN32)

 AFF4Status NewFileBackedObject(
     DataStore *resolver,
     std::string filename,
     std::string mode,
     AFF4Flusher<FileBackedObject> &result) {
     auto new_object = AFF4Flusher<FileBackedObject>(new FileBackedObject(resolver));
     new_object->urn = URN::NewURNFromFilename(filename, false);

    std::vector<std::string> directory_components = split(filename, PATH_SEP);
    directory_components.pop_back();

    DWORD creation_disposition = OPEN_EXISTING;
    DWORD desired_access = GENERIC_READ;

    if (mode == "truncate") {
        creation_disposition = CREATE_ALWAYS;
        desired_access |= GENERIC_WRITE;

        new_object->properties.writable = true;

        // Only create directories if we are allowed to.
        RETURN_IF_ERROR(
            CreateIntermediateDirectories(resolver, directory_components));

    } else if (mode == "append") {
        creation_disposition = OPEN_ALWAYS;
        desired_access |= GENERIC_WRITE;
        new_object->properties.writable = true;

        // Only create directories if we are allowed to.
        RETURN_IF_ERROR(
            CreateIntermediateDirectories(resolver, directory_components));
    }

    new_object->fd = CreateFile(
        filename.c_str(),
        desired_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        creation_disposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (new_object->fd == INVALID_HANDLE_VALUE) {
        resolver->logger->error(
            "Cannot open file {} : {}", filename,
            GetLastErrorMessage());

        return IO_ERROR;
    }

    LARGE_INTEGER tmp;

    // Now deduce the size of the stream.
    if (GetFileSizeEx(new_object->fd, &tmp)) {
        new_object->size = tmp.QuadPart;
    } else {
        // The file may be a raw device so we need to issue an ioctl to see how
        // large it is.
        GET_LENGTH_INFORMATION lpOutBuffer;
        DWORD lpBytesReturned;
        if (DeviceIoControl(
                    new_object->fd,                // handle to device
                    IOCTL_DISK_GET_LENGTH_INFO,    // dwIoControlCode
                    nullptr,                          // lpInBuffer
                    0,                             // nInBufferSize
                    &lpOutBuffer,                     // output buffer
                    sizeof(GET_LENGTH_INFORMATION),
                    (LPDWORD) &lpBytesReturned,    // number of bytes returned
                    nullptr)) {
            new_object->size = lpOutBuffer.Length.QuadPart;
        } else {
            // We dont know the size - seek relative to the end will fail now.
            new_object->size = -1;
            new_object->properties.sizeable = false;
        }
    }

    result.swap(new_object);

    return STATUS_OK;
}

AFF4Status FileBackedObject::ReadBuffer(char* data, size_t *length) {
    DWORD buf_length = (DWORD)*length;

    if (properties.seekable) {
        LARGE_INTEGER tmp;
        tmp.QuadPart = readptr;
        if (!SetFilePointerEx(fd, tmp, &tmp, FILE_BEGIN)) {
            resolver->logger->info("Failed to seek: {}", GetLastErrorMessage());
            return IO_ERROR;
        }
    }

    if (!ReadFile(fd, data, buf_length, &buf_length, nullptr)) {
        resolver->logger->warn("Reading failed at {:x}: {}", readptr,
                                GetLastErrorMessage());

        return IO_ERROR;
    }

    *length = buf_length;
    readptr += *length;

    return STATUS_OK;
}

AFF4Status FileBackedObject::Write(const char* data, size_t length) {
    // Dont ever try to write on files we are not allowed to write on.
    if (!properties.writable) {
        return IO_ERROR;
    }

    if (properties.seekable) {
        LARGE_INTEGER tmp;
        tmp.QuadPart = readptr;
        SetFilePointerEx(fd, tmp, &tmp, FILE_BEGIN);
    }

    DWORD tmp = length;
    while (!WriteFile(fd, data, tmp, &tmp, nullptr)) {
        resolver->logger->warn("Unable to write to disk. Is it full? "
                  "Please try to free space to continue.");
        Sleep(1000);
    }

    readptr += tmp;
    if (size >= 0 && readptr > size) {
        size = readptr;
    }

    return STATUS_OK;
}

AFF4Status FileBackedObject::Truncate() {
    if (!properties.seekable) {
        return IO_ERROR;
    }

    LARGE_INTEGER tmp;
    tmp.QuadPart = 0;

    SetFilePointerEx(fd, tmp, &tmp, FILE_BEGIN);
    if (SetEndOfFile(fd) == 0) {
        return IO_ERROR;
    }

    return STATUS_OK;
}

FileBackedObject::~FileBackedObject() {
    CloseHandle(fd);
}

// On other systems the posix open() API is used.
#else

 AFF4Status NewFileBackedObject(
     DataStore *resolver,
     std::string filename,
     std::string mode,
     AFF4Flusher<FileBackedObject> &result
 ) {
     auto new_object = AFF4Flusher<FileBackedObject>(new FileBackedObject(resolver));
     new_object->urn = URN::NewURNFromFilename(filename, false);

     std::vector<std::string> directory_components = split(filename, PATH_SEP);
     directory_components.pop_back();

     auto flags = O_RDONLY;

     if (mode == "truncate" ) {
         flags = O_TRUNC | O_CREAT | O_RDWR;
         new_object->properties.writable = true;

         // Only create directories if we are allowed to.
         RETURN_IF_ERROR(
             CreateIntermediateDirectories(resolver, directory_components));

     } else if (mode == "append") {
         flags = O_CREAT | O_RDWR;
         new_object->properties.writable = true;

         // Only create directories if we are allowed to.
         RETURN_IF_ERROR(
             CreateIntermediateDirectories(resolver, directory_components));
     }

    resolver->logger->debug("Opening file {}", filename);

    new_object->fd = open(filename.c_str(), flags,
              S_IRWXU | S_IRWXG | S_IRWXO);

    if (new_object->fd < 0) {
        resolver->logger->error("Cannot open file {}: {}", filename,
                                GetLastErrorMessage());
        return IO_ERROR;
    }

    // If this fails we dont know the size - this can happen e.g. with devices. In
    // this case seeks relative to the end will fail.
    new_object->size = lseek(new_object->fd, 0, SEEK_END);
    if (new_object->size < 0) {
        new_object->properties.sizeable = false;
    }

    // Detect if the file is seekable (e.g. a pipe).
    if (lseek(new_object->fd, 0, SEEK_CUR) < 0) {
        new_object->properties.seekable = false;
    }

     result.swap(new_object);

     return STATUS_OK;
 }

AFF4Status FileBackedObject::ReadBuffer(char* data, size_t *length) {
    lseek(fd, readptr, SEEK_SET);
    const int res = read(fd, data, *length);
    if (res < 0) {
        *length = 0;
        return IO_ERROR;
    }

    readptr += res;
    *length = res;
    return STATUS_OK;
}

AFF4Status FileBackedObject::Write(const char* data, size_t length) {
    if (!properties.writable) {
        return IO_ERROR;
    }

    // Since all file operations are synchronous this object cannot be dirty.
    if (properties.seekable) {
        lseek(fd, readptr, SEEK_SET);
    }

    int res = write(fd, data, length);
    if (res >= 0) {
        readptr += res;
    } else {
        return IO_ERROR;
    }

    if (readptr > size) {
        size = readptr;
    }
    resolver->logger->debug("Writing {} on {}/{}", length, readptr, size);
    return STATUS_OK;
}

AFF4Status FileBackedObject::Truncate() {
    if (ftruncate(fd, 0) != 0) {
        return IO_ERROR;
    }

    RETURN_IF_ERROR(Seek(0, SEEK_SET));
    size = 0;

    return STATUS_OK;
}


FileBackedObject::~FileBackedObject() {
    if (fd >= 0) {
        close(fd);
    }
}

#endif


AFF4Status AFF4Stdout::NewAFF4Stdout(
        DataStore *resolver,
        AFF4Flusher<AFF4Stream> &result) {

    AFF4Flusher<AFF4Stdout> new_object(new AFF4Stdout(resolver));

    new_object->properties.seekable = false;
    new_object->properties.writable = true;
    new_object->properties.sizeable = false;

#ifdef _WIN32
    // On windows stdout is set to text mode, we need to force it
    // to binary mode or else it will corrupt the output (issue
    // #31)
    new_object->fd = _fileno( stdout );
    _setmode (new_object->fd, _O_BINARY);
#else
    new_object->fd = STDOUT_FILENO;
#endif

    result.reset(new_object.release());

    return STATUS_OK;
}

AFF4Status AFF4Stdout::Write(const char* data, size_t length) {
    int res = write(fd, data, length);
    if (res >= 0) {
        readptr += res;
    } else {
        return IO_ERROR;
    }

    if (readptr > size) {
        size = readptr;
    }

    return STATUS_OK;
}

AFF4Status AFF4Stdout::Truncate() {
    return IO_ERROR;
}


AFF4Status AFF4Stdout::Seek(aff4_off_t offset, int whence) {
    if (whence == SEEK_END && offset == 0) {
        return STATUS_OK;
    }

    if (whence == SEEK_SET && offset == 0) {
        return STATUS_OK;
    }

    return IO_ERROR;
}


} // namespace aff4
