# Client Updates

English: [Overview](#overview) | [Repository Setting](#repository-setting) | [GitHub Release Flow](#github-release-flow) | [Manifest Format](#manifest-format) | [Examples](#examples) | [Platform Install Behavior](#platform-install-behavior) | [Release Checklist](#release-checklist)

Русский: [Обзор](#обзор) | [Настройка репозитория](#настройка-репозитория) | [Сценарий GitHub Release](#сценарий-github-release) | [Формат manifest](#формат-manifest) | [Примеры](#примеры) | [Поведение установки по платформам](#поведение-установки-по-платформам) | [Чеклист релиза](#чеклист-релиза)

## Overview

The client update flow is driven by `UpdateManager` in the desktop and Android client.

The updater supports two repository formats:

1. A GitHub repository in `owner/repo` form.
2. A direct `http://` or `https://` URL pointing to a manifest JSON file.

In both cases, the final source of truth is a JSON manifest that contains:

1. The latest application version.
2. The optional protocol version for compatibility checks.
3. Release notes.
4. A downloadable artifact for the current platform.

If the manifest is missing a version or does not contain an artifact for the current platform, the update check fails.

At the moment, the built-in update flow documented in this file covers Linux AppImage, portable Windows `.exe`, and Android `.apk`. macOS desktop updates are planned for later and are not yet part of the documented production workflow.

## Repository Setting

The client reads the update source from the `updatesRepository` text setting.

Examples:

```text
your-org/patronus
```

```text
https://downloads.example.com/patronus/manifest.json
```

Behavior:

1. If the value starts with `http://` or `https://`, the client treats it as a direct manifest URL.
2. Otherwise, the client treats it as a GitHub repository slug and queries the GitHub API for the latest release.

## GitHub Release Flow

When `updatesRepository` is set to `owner/repo`, the client requests:

```text
https://api.github.com/repos/owner/repo/releases/latest
```

The latest release must contain an asset named exactly:

```text
manifest.json
```

The updater searches the asset list of the latest release, finds `manifest.json`, downloads it, and then chooses the correct artifact for the current platform.

Important consequences:

1. The asset name `manifest.json` is mandatory when you use GitHub releases.
2. Artifact file names are otherwise flexible because the manifest points to the artifact URL explicitly.
3. The manifest may point to GitHub release assets, another HTTPS host, or any other reachable download URL.

## Manifest Format

The manifest must be a JSON object.

### Required data

The updater accepts any of these keys for the application version:

1. `version`
2. `appVersion`
3. `latestVersion`

At least one of them must be present and non-empty.

### Optional protocol version

The updater also understands these keys:

1. `protocolVersion`
2. `protocol_version`

This field is optional.

If it is present and does not match the client's compiled protocol version, the client marks a protocol mismatch. At the time of writing, the current protocol version in this repository is:

```text
1
```

### Optional release notes

The updater accepts release notes from:

1. `notes`
2. `releaseNotes`
3. `description`

`notes` may be either:

1. A string.
2. An array of strings or primitive values that can be converted to strings.

### Artifact selection

The updater selects artifacts by platform key.

Recognized runtime platforms are:

1. `linux`
2. `windows`
3. `android`

For Linux, `appimage` is also accepted as a synonym during manifest lookup.

An artifact object is valid when it contains a non-empty download URL in one of these fields:

1. `url`
2. `browser_download_url`
3. `downloadUrl`

Optional file name fields are:

1. `fileName`
2. `filename`
3. `name`

### Supported manifest layouts

The updater supports three layouts.

#### 1. `artifacts` as an object

```json
{
  "version": "1.2.0",
  "protocolVersion": "1",
  "notes": "Bug fixes and protocol hardening.",
  "artifacts": {
    "linux": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-x86_64.AppImage",
      "fileName": "patronus-x86_64.AppImage"
    },
    "windows": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus.exe",
      "fileName": "patronus.exe"
    },
    "android": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-release.apk",
      "fileName": "patronus-release.apk"
    }
  }
}
```

#### 2. `artifacts` as an array

```json
{
  "version": "1.2.0",
  "protocol_version": "1",
  "releaseNotes": [
    "Improved delivery reliability",
    "Fixed update download edge cases"
  ],
  "artifacts": [
    {
      "platform": "linux",
      "url": "https://downloads.example.com/patronus/patronus-x86_64.AppImage",
      "name": "patronus-x86_64.AppImage"
    },
    {
      "platform": "windows",
      "url": "https://downloads.example.com/patronus/patronus.exe",
      "name": "patronus.exe"
    },
    {
      "platform": "android",
      "url": "https://downloads.example.com/patronus/patronus-release.apk",
      "name": "patronus-release.apk"
    }
  ]
}
```

#### 3. Top-level platform keys

```json
{
  "version": "1.2.0",
  "description": "Small maintenance release.",
  "appimage": {
    "url": "https://downloads.example.com/patronus/patronus-x86_64.AppImage",
    "fileName": "patronus-x86_64.AppImage"
  },
  "windows": {
    "url": "https://downloads.example.com/patronus/patronus.exe",
    "fileName": "patronus.exe"
  },
  "android": {
    "url": "https://downloads.example.com/patronus/patronus-release.apk",
    "fileName": "patronus-release.apk"
  }
}
```

## Examples

### Minimal GitHub release layout

Latest GitHub release assets:

1. `manifest.json`
2. `patronus-x86_64.AppImage`
3. `patronus.exe`
4. `patronus-release.apk`

Example `manifest.json` uploaded as a release asset:

```json
{
  "version": "1.2.0",
  "protocolVersion": "1",
  "notes": [
    "New release channel build.",
    "Updated Linux and Android packages."
  ],
  "artifacts": {
    "linux": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-x86_64.AppImage",
      "fileName": "patronus-x86_64.AppImage"
    },
    "windows": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus.exe",
      "fileName": "patronus.exe"
    },
    "android": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-release.apk",
      "fileName": "patronus-release.apk"
    }
  }
}
```

### Direct manifest URL layout

If you do not want to depend on GitHub releases, host the manifest yourself and point `updatesRepository` directly to it.

Example:

```text
https://downloads.example.com/patronus/stable/manifest.json
```

In that mode, the file does not need to be named `manifest.json`, but using that name still makes the setup easier to understand.

## Platform Install Behavior

The client downloads the selected update artifact into its application data area under an `updates` directory.

On desktop builds in this repository, application data is kept under a portable directory next to the executable or AppImage:

```text
PatronusData/files/updates/
```

Platform-specific installation behavior:

1. Linux: the client expects an AppImage-like replacement flow. It writes a temporary shell script, waits for the old process to exit, copies the downloaded file over the target AppImage, marks it executable, relaunches it, and removes the helper script.
2. Windows: the client writes a temporary `.cmd` helper, waits for the running process to exit, copies the downloaded file over the current `.exe`, relaunches the updated executable, and removes the helper script.
3. Android: the client opens the system package installer for the downloaded APK.

Practical release implications:

1. For Linux, publish an AppImage if you want the built-in install flow to be smooth.
2. For Windows, publish a replacement-ready portable `.exe`, because the built-in flow now replaces the currently running executable instead of launching a separate installer.
3. For Android, publish an installable `.apk`.
4. For macOS, do not rely on this document as a finished updater contract yet. macOS update support will be implemented later.

On Windows, this also means the application directory must be writable by the current user. If the executable is installed into a protected location such as `Program Files`, in-place replacement may fail without elevated permissions.

## Release Checklist

For a GitHub-based update channel:

1. Build the platform artifacts you want to distribute.
2. Create a new GitHub release.
3. Upload the artifacts.
4. Upload an asset named exactly `manifest.json`.
5. Make sure the manifest points to the correct downloadable URLs.
6. Make sure the manifest version is newer than the version compiled into the currently installed client.
7. If you use `protocolVersion`, keep it aligned with the protocol actually required by the release.

Operational notes:

1. Version comparison is numeric when possible, so `1.10.0` is newer than `1.2.0`.
2. If the parsed numeric versions are equal or unavailable, the updater falls back to string inequality.
3. Redirects are allowed only when they are not less safe than the original request.
4. For production, serve manifests and artifacts over valid HTTPS.

## Обзор

Механизм обновлений клиента управляется классом `UpdateManager` в десктопном и Android-клиенте.

Updater поддерживает два формата источника обновлений:

1. GitHub-репозиторий в виде `owner/repo`.
2. Прямой `http://` или `https://` URL на manifest JSON-файл.

В обоих случаях конечным источником истины является JSON manifest, в котором есть:

1. Последняя версия приложения.
2. Опциональная версия протокола для проверки совместимости.
3. Release notes.
4. Ссылка на скачиваемый артефакт для текущей платформы.

Если в manifest нет версии или нет артефакта для текущей платформы, проверка обновлений завершается ошибкой.

На данный момент встроенный flow обновлений, описанный в этом файле, покрывает Linux AppImage, portable Windows `.exe` и Android `.apk`. Обновления для macOS будут реализованы позже и пока не входят в задокументированный production workflow.

## Настройка репозитория

Клиент читает источник обновлений из текстовой настройки `updatesRepository`.

Примеры:

```text
your-org/patronus
```

```text
https://downloads.example.com/patronus/manifest.json
```

Поведение:

1. Если значение начинается с `http://` или `https://`, клиент считает, что это прямой URL на manifest.
2. Иначе клиент считает, что это slug GitHub-репозитория, и запрашивает latest release через GitHub API.

## Сценарий GitHub Release

Когда `updatesRepository` задан как `owner/repo`, клиент запрашивает:

```text
https://api.github.com/repos/owner/repo/releases/latest
```

В latest release обязательно должен лежать asset с точным именем:

```text
manifest.json
```

Updater проходит по списку assets у latest release, находит `manifest.json`, скачивает его и затем выбирает подходящий артефакт для текущей платформы.

Что из этого следует:

1. Имя asset `manifest.json` обязательно, если вы используете GitHub releases.
2. Имена остальных файлов могут быть любыми, потому что manifest явно указывает URL артефакта.
3. Manifest может ссылаться как на GitHub release assets, так и на другой HTTPS-host или любой другой доступный download URL.

## Формат manifest

Manifest должен быть JSON-объектом.

### Обязательные данные

Updater принимает любое из этих полей как версию приложения:

1. `version`
2. `appVersion`
3. `latestVersion`

Хотя бы одно из них должно присутствовать и быть непустым.

### Опциональная версия протокола

Updater также понимает поля:

1. `protocolVersion`
2. `protocol_version`

Это поле необязательное.

Если оно присутствует и не совпадает со скомпилированной в клиент версии протокола, клиент помечает protocol mismatch. На момент написания текущая версия протокола в этом репозитории такая:

```text
1
```

### Опциональные release notes

Updater принимает release notes из:

1. `notes`
2. `releaseNotes`
3. `description`

Поле `notes` может быть:

1. Строкой.
2. Массивом строк или примитивных значений, которые можно преобразовать в строки.

### Выбор артефакта

Updater выбирает артефакт по ключу платформы.

Распознаваемые runtime-платформы:

1. `linux`
2. `windows`
3. `android`

Для Linux во время поиска manifest также принимается `appimage` как синоним.

Объект артефакта считается валидным, если в нём есть непустой download URL в одном из полей:

1. `url`
2. `browser_download_url`
3. `downloadUrl`

Опциональные поля имени файла:

1. `fileName`
2. `filename`
3. `name`

### Поддерживаемые layout manifest

Updater поддерживает три layout.

#### 1. `artifacts` как объект

```json
{
  "version": "1.2.0",
  "protocolVersion": "1",
  "notes": "Bug fixes and protocol hardening.",
  "artifacts": {
    "linux": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-x86_64.AppImage",
      "fileName": "patronus-x86_64.AppImage"
    },
    "windows": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus.exe",
      "fileName": "patronus.exe"
    },
    "android": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-release.apk",
      "fileName": "patronus-release.apk"
    }
  }
}
```

#### 2. `artifacts` как массив

```json
{
  "version": "1.2.0",
  "protocol_version": "1",
  "releaseNotes": [
    "Improved delivery reliability",
    "Fixed update download edge cases"
  ],
  "artifacts": [
    {
      "platform": "linux",
      "url": "https://downloads.example.com/patronus/patronus-x86_64.AppImage",
      "name": "patronus-x86_64.AppImage"
    },
    {
      "platform": "windows",
      "url": "https://downloads.example.com/patronus/patronus.exe",
      "name": "patronus.exe"
    },
    {
      "platform": "android",
      "url": "https://downloads.example.com/patronus/patronus-release.apk",
      "name": "patronus-release.apk"
    }
  ]
}
```

#### 3. Платформенные ключи на верхнем уровне

```json
{
  "version": "1.2.0",
  "description": "Small maintenance release.",
  "appimage": {
    "url": "https://downloads.example.com/patronus/patronus-x86_64.AppImage",
    "fileName": "patronus-x86_64.AppImage"
  },
  "windows": {
    "url": "https://downloads.example.com/patronus/patronus.exe",
    "fileName": "patronus.exe"
  },
  "android": {
    "url": "https://downloads.example.com/patronus/patronus-release.apk",
    "fileName": "patronus-release.apk"
  }
}
```

## Примеры

### Минимальная структура GitHub release

Assets у latest GitHub release:

1. `manifest.json`
2. `patronus-x86_64.AppImage`
3. `patronus.exe`
4. `patronus-release.apk`

Пример `manifest.json`, загруженного как asset релиза:

```json
{
  "version": "1.2.0",
  "protocolVersion": "1",
  "notes": [
    "New release channel build.",
    "Updated Linux and Android packages."
  ],
  "artifacts": {
    "linux": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-x86_64.AppImage",
      "fileName": "patronus-x86_64.AppImage"
    },
    "windows": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus.exe",
      "fileName": "patronus.exe"
    },
    "android": {
      "url": "https://github.com/owner/repo/releases/download/v1.2.0/patronus-release.apk",
      "fileName": "patronus-release.apk"
    }
  }
}
```

### Сценарий с прямым URL на manifest

Если вы не хотите завязываться на GitHub releases, можно хостить manifest самостоятельно и указывать `updatesRepository` напрямую на него.

Пример:

```text
https://downloads.example.com/patronus/stable/manifest.json
```

В этом режиме файл не обязан называться `manifest.json`, но это имя всё равно делает схему проще и понятнее.

## Поведение установки по платформам

Клиент скачивает выбранный update artifact в application data area в директорию `updates`.

В desktop build этого репозитория application data хранится в portable directory рядом с executable или AppImage:

```text
PatronusData/files/updates/
```

Поведение установки по платформам:

1. Linux: клиент ожидает AppImage-подобный сценарий замены. Он пишет временный shell script, ждёт завершения старого процесса, копирует скачанный файл поверх целевого AppImage, делает его исполняемым, перезапускает его и удаляет вспомогательный скрипт.
2. Windows: клиент пишет временный `.cmd` helper, ждёт завершения текущего процесса, копирует скачанный файл поверх текущего `.exe`, перезапускает обновлённый executable и удаляет вспомогательный скрипт.
3. Android: клиент открывает системный package installer для скачанного APK.

Практические последствия для релизов:

1. Для Linux публикуйте AppImage, если хотите, чтобы встроенный install flow работал гладко.
2. Для Windows публикуйте portable `.exe`, пригодный для прямой замены, потому что встроенный flow теперь подменяет текущий executable, а не запускает отдельный installer.
3. Для Android публикуйте installable `.apk`.
4. Для macOS пока не стоит воспринимать этот документ как завершённый контракт обновлений. Поддержка обновлений для macOS будет реализована позже.

На Windows это также означает, что директория приложения должна быть доступна текущему пользователю на запись. Если executable установлен в защищённое место вроде `Program Files`, in-place замена может не получиться без повышения прав.

## Чеклист релиза

Для GitHub-based update channel:

1. Соберите платформенные артефакты, которые хотите раздавать.
2. Создайте новый GitHub release.
3. Загрузите артефакты.
4. Загрузите asset с точным именем `manifest.json`.
5. Проверьте, что manifest указывает на корректные download URL.
6. Убедитесь, что версия в manifest новее версии, скомпилированной в уже установленный клиент.
7. Если используете `protocolVersion`, держите его синхронизированным с реально требуемой версией протокола.

Операционные замечания:

1. Сравнение версий по возможности числовое, поэтому `1.10.0` считается новее, чем `1.2.0`.
2. Если числовые версии равны или не распарсились, updater использует fallback на неравенство строк.
3. Redirects разрешены только если они не менее безопасны, чем исходный запрос.
4. Для production раздавайте manifest и артефакты по валидному HTTPS.