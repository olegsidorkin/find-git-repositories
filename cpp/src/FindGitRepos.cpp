#include <napi.h>
#include <chrono>
#include <list>
#include <vector>
#include "../includes/Queue.h"
#if defined(_WIN32)
#include "../includes/WindowsHelpers.h"
#else
#include <uv.h>
#endif

class FindGitReposWorker: public Napi::AsyncWorker {
public:
  FindGitReposWorker(
    Napi::Env env,
    std::string _path,
    std::shared_ptr<RepositoryQueue> _progressQueue,
    Napi::ThreadSafeFunction _progressCallback,
    uint32_t _throttleTimeoutMS
  ):
    Napi::AsyncWorker(env),
    deferred(Napi::Promise::Deferred::New(env)),
    path(_path),
    progressQueue(_progressQueue),
    progressCallback(_progressCallback),
    throttleTimeoutMS(_throttleTimeoutMS),
    lastProgressCallbackTimePoint(std::chrono::steady_clock::now())
  {
    lastProgressCallbackTimePoint = lastProgressCallbackTimePoint - throttleTimeoutMS;
  }

  ~FindGitReposWorker() {
    progressCallback.Release();
  }

  #if defined(_WIN32)
  void Execute() {
    const std::wstring gitPath = L".git";
    const std::wstring dot = L".";
    const std::wstring dotdot = L"..";
    std::list<std::wstring> foundPaths;
    auto rootPath = convertMultiByteToWideChar(path);
    const bool wasNtPath = isNtPath(rootPath);

    if (!wasNtPath) {
      while (!rootPath.empty() && rootPath.back() == L'\\') {
        rootPath.pop_back();
      }
      if (rootPath.empty()) {
        return;
      }
      rootPath = prefixWithNtPath(rootPath);
    }

    foundPaths.push_back(rootPath);

    while (foundPaths.size()) {
      WIN32_FIND_DATAW FindFileData;
      HANDLE hFind = INVALID_HANDLE_VALUE;
      std::wstring currentPath = foundPaths.front();
      foundPaths.pop_front();

      std::wstring wildcardPath = currentPath + L"\\*";
      hFind = FindFirstFileW(wildcardPath.c_str(), &FindFileData);
      if (hFind == INVALID_HANDLE_VALUE) {
        continue;
      }

      std::list<std::wstring> tempPaths;
      if (
        (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY
        && dot != FindFileData.cFileName
        && dotdot != FindFileData.cFileName
      ) {
        if (gitPath == FindFileData.cFileName) {
          std::string repoPath;
          int success = convertWideCharToMultiByte(&repoPath, currentPath, wasNtPath);
          if (!success) {
            FindClose(hFind);
            continue;
          }

          repoPath += "\\.git";

          progressQueue->enqueue(repoPath);
          repositories.push_back(repoPath);
          ThrottledProgressCallback();
          FindClose(hFind);
          continue;
        }

        tempPaths.push_back(currentPath + L"\\" + std::wstring(FindFileData.cFileName));
      }

      bool isGitRepo = false;
      while (FindNextFileW(hFind, &FindFileData)) {
        if (dot == FindFileData.cFileName || dotdot == FindFileData.cFileName) {
          continue;
        }

        if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }

        if (gitPath == FindFileData.cFileName) {
          std::string repoPath;
          int success = convertWideCharToMultiByte(&repoPath, currentPath, wasNtPath);
          isGitRepo = true;
          if (!success) {
            break;
          }

          repoPath += "\\.git";

          progressQueue->enqueue(repoPath);
          repositories.push_back(repoPath);
          ThrottledProgressCallback();
          break;
        }

        tempPaths.push_back(currentPath + L"\\" + std::wstring(FindFileData.cFileName));
      }

      if (!isGitRepo) {
        foundPaths.splice(foundPaths.end(), tempPaths);
      }

      FindClose(hFind);
    }
  }
  #else
  void Execute() {
    uv_dirent_t directoryEntry;
    uv_fs_t scandirRequest;
    std::list<std::string> foundPaths;
    foundPaths.push_back(path);

    while (foundPaths.size()) {
      std::list<std::string> temp;
      bool isGitRepo = false;
      std::string currentPath = foundPaths.front();
      foundPaths.pop_front();

      if (uv_fs_scandir(NULL, &scandirRequest, (currentPath + '/').c_str(), 0, NULL) < 0) {
        continue;
      }

      while (uv_fs_scandir_next(&scandirRequest, &directoryEntry) != UV_EOF) {
        std::string nextPath = currentPath + '/' + directoryEntry.name;

        if (directoryEntry.type == UV_DIRENT_UNKNOWN) {
          uv_fs_t lstatRequest;
          if (
            uv_fs_lstat(NULL, &lstatRequest, nextPath.c_str(), NULL) < 0
            || !S_ISDIR(lstatRequest.statbuf.st_mode)
            || S_ISLNK(lstatRequest.statbuf.st_mode)
          ) {
            continue;
          }
        } else if (directoryEntry.type != UV_DIRENT_DIR) {
          continue;
        }

        if (strcmp(directoryEntry.name, ".git")) {
          temp.push_back(nextPath);
          continue;
        }

        isGitRepo = true;
        progressQueue->enqueue(nextPath);
        repositories.push_back(nextPath);
        ThrottledProgressCallback();
      }

      if (!isGitRepo) {
        foundPaths.splice(foundPaths.end(), temp);
      }
    }
  }
  #endif

  void OnOK() {
    Napi::Env env = Env();
    size_t numRepos = repositories.size();
    Napi::Array repositoryArray = Napi::Array::New(env, numRepos);
    for (size_t i = 0; i < numRepos; ++i) {
      repositoryArray[i] = Napi::String::New(env, repositories[i]);
    }

    deferred.Resolve(repositoryArray);
  }

  Napi::Promise Promise() {
    return deferred.Promise();
  }

  void ThrottledProgressCallback() {
    auto callback = [](Napi::Env env, Napi::Function jsCallback, RepositoryQueue *progressQueue) {
      int numRepos = progressQueue->count();
      if (numRepos == 0) {
        return;
      }

      Napi::Array repositoryArray = Napi::Array::New(env, numRepos);
      for (int i = 0; i < numRepos; ++i) {
        // TODO upgrade progressQueue to be type safe here
        repositoryArray[(uint32_t)i] = Napi::String::New(env, progressQueue->dequeue());
      }

      jsCallback.Call({ repositoryArray });
    };

    if (throttleTimeoutMS.count() == 0) {
      progressCallback.NonBlockingCall(progressQueue.get(), callback);
      return;
    }

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now - lastProgressCallbackTimePoint < throttleTimeoutMS) {
      return;
    }

    progressCallback.NonBlockingCall(progressQueue.get(), callback);
    lastProgressCallbackTimePoint = now;
  }

private:
  Napi::Promise::Deferred deferred;
  std::string path;
  std::shared_ptr<RepositoryQueue> progressQueue;
  Napi::ThreadSafeFunction progressCallback;
  std::chrono::milliseconds throttleTimeoutMS;
  std::chrono::steady_clock::time_point lastProgressCallbackTimePoint;
  std::vector<std::string> repositories;
};

Napi::Promise FindGitRepos(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString() || info[0].ToString().Utf8Value().empty()) {
    Napi::Promise::Deferred deferred(env);
    deferred.Reject(Napi::TypeError::New(env, "Must provide non-empty starting path as first argument.").Value());
    return deferred.Promise();
  }

  if (info.Length() < 2 || !info[1].IsFunction()) {
    Napi::Promise::Deferred deferred(env);
    deferred.Reject(Napi::TypeError::New(env, "Must provide progress callback as second argument.").Value());
    return deferred.Promise();
  }

  uint32_t throttleTimeoutMS = 0;
  if (info.Length() >= 3) {
    if (!info[2].IsObject()) {
      Napi::Promise::Deferred deferred(env);
      deferred.Reject(Napi::TypeError::New(env, "Options argument must be an object, if passed.").Value());
      return deferred.Promise();

    }

    Napi::Object options = info[2].ToObject();
    Napi::Value maybeThrottleTimeoutMS = options["throttleTimeoutMS"];
    if (options.Has("throttleTimeoutMS") && !maybeThrottleTimeoutMS.IsNumber()) {
      Napi::Promise::Deferred deferred(env);
      deferred.Reject(Napi::TypeError::New(env, "options.throttleTimeoutMS must be a number, if passed.").Value());
      return deferred.Promise();
    }

    if (maybeThrottleTimeoutMS.IsNumber()) {
      Napi::Number temp = maybeThrottleTimeoutMS.ToNumber();
      double bounds = temp.DoubleValue();
      if (bounds < 0 || bounds > 60000) {
        Napi::Promise::Deferred deferred(env);
        deferred.Reject(Napi::TypeError::New(env, "options.throttleTimeoutMS must be > 0 and <= 60000, if passed.").Value());
        return deferred.Promise();
      }

      throttleTimeoutMS = temp;
    }
  }

  std::shared_ptr<RepositoryQueue> progressQueue(new RepositoryQueue);
  Napi::ThreadSafeFunction progressCallback = Napi::ThreadSafeFunction::New(
    env,
    info[1].As<Napi::Function>(),
    "findGitRepos",
    0,
    1,
    [progressQueue](Napi::Env env) {}
  );

  FindGitReposWorker *worker = new FindGitReposWorker(info.Env(), info[0].ToString(), progressQueue, progressCallback, throttleTimeoutMS);
  worker->Queue();

  return worker->Promise();
}


Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return Napi::Function::New(env, FindGitRepos);
}

NODE_API_MODULE(hello, Init)
