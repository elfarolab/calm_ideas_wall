# Calm Ideas Wall (C++)

A small production-oriented C++ REST service for managing a limited set of project ideas.

It is built around:

- [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) for HTTP
- [`RapidJSON`](https://github.com/Tencent/rapidjson) for JSON parsing/serialization
- [`spdlog`](https://github.com/gabime/spdlog) for logging

Data is persisted in a local JSONL file:

```text
projects.jsonl
```

The app is intended to run as a local backend behind a reverse proxy such as nginx.

---

## Features

- JSONL persistence
- Configurable maximum number of total projects
- Configurable maximum number of `growing` projects
- Configuration file support
- Command line overrides for `host`, `port`, and config file path
- Project statuses:
  - `seed`
  - `growing`
  - `parked`
- Project priorities:
  - `low`
  - `medium`
  - `high`
- File logging
- Serves `index.html` at `/`
- Serves static assets from `/assets`
- Designed to run behind nginx

---

## API

### Core endpoints

- `GET /`
- `GET /api/meta`
- `GET /api/projects`
- `POST /api/projects`
- `GET /api/projects/{id}`
- `PUT /api/projects/{id}`
- `DELETE /api/projects/{id}`
- `POST /api/projects/{id}/view`
- `POST /api/projects/{id}/notes`

### Optional query parameters for `GET /api/projects`

- `status=seed|growing|parked|all`
- `q=<search text>`

---

## Configuration

The application reads a small flat configuration file named:

```text
config
```

By default, it looks for this file in the same directory as the application data files.

In development, this is usually the project root:

```text
./config
```

In the recommended production layout, this is:

```text
/opt/calm_ideas_wall/config
```

You can override the config file location with:

```sh
--config /path/to/config
```

### Config file format

The config file uses a simple `key: value` format.

Example:

```text
# Network settings
host: 127.0.0.1
port: 8080

# Limits
max_projects: 10
max_growing: 3
```

### Supported keys

| Key            | Description                                  | Default   |
|----------------|----------------------------------------------|-----------|
| `host`         | Interface to bind the HTTP server to         | `0.0.0.0` |
| `port`         | TCP port to listen on                        | `8000`    |
| `max_projects` | Maximum number of total projects allowed     | `10`      |
| `max_growing`  | Maximum number of `growing` projects allowed | `3`       |

### Notes

- Missing config file: the app starts using defaults.
- Missing keys: those keys use defaults.
- Unknown keys: ignored.
- Comments are supported with `#`.
- The parser is intentionally simple and supports the four keys above.
- Invalid values for `port`, `max_projects`, or `max_growing` cause startup to fail.

---

## Option priority

The effective runtime values are resolved in this order.

### `host`

1. Command line: `--host`
2. Config file: `host`
3. Default: `0.0.0.0`

### `port`

1. Command line: `--port`
2. Config file: `port`
3. Environment variable: `PORT`
4. Default: `8000`

### `max_projects`

1. Config file: `max_projects`
2. Default: `10`

### `max_growing`

1. Config file: `max_growing`
2. Default: `3`

In short:

```text
command line > config file > environment variable > built-in default
```

For `max_projects` and `max_growing`, only config file and default are used.

---

## Command line options

```text
--host <host>      Bind address to listen on
--port <port>      TCP port to listen on
--config <path>    Path to the config file
-h, --help         Show usage
```

You can also use the `=` form:

```sh
./build/calm_ideas_wall --host=127.0.0.1 --port=8080
./build/calm_ideas_wall --config=/opt/calm_ideas_wall/config
```

### Examples

Use the default config file:

```sh
./build/calm_ideas_wall
```

Override only the port:

```sh
./build/calm_ideas_wall --port 9000
```

Override only the host:

```sh
./build/calm_ideas_wall --host 127.0.0.1
```

Override both:

```sh
./build/calm_ideas_wall --host 127.0.0.1 --port 9000
```

Use a different config file:

```sh
./build/calm_ideas_wall --config /opt/calm_ideas_wall/config
```

Show help:

```sh
./build/calm_ideas_wall --help
```

---

## Sample projects files

A sample projects file is provide into ./sample subdir.
You can use the import button and its functions to upload a sample set of projects for rapid evaluation and play around.

---

## Requirements

This project expects the required dependencies to be vendored into the `libs/` directory.

### Required libraries

- `cpp-httplib`
- `RapidJSON`
- `spdlog`

### Required build tools

- `cmake`
- `ninja`
- A C++17 compiler

---

## Expected `libs/` layout

```text
libs/
├── httplib/
│   └── include/
│       └── httplib.hpp
├── rapidjson/
│   └── include/
│       └── rapidjson/
│           ├── document.h
│           ├── writer.h
│           ├── stringbuffer.h
│           └── ...
└── spdlog/
    └── include/
        └── spdlog/
            ├── spdlog.h
            ├── sinks/
            │   ├── basic_file_sink.h
            │   └── stdout_color_sinks.h
            └── ...
```

If you do not already have these libraries vendored, you can place them into `libs/` before building.

---

## Building with Make

From the project root:

```sh
make build
```

This runs CMake with Ninja and builds the executable into the `build/` directory.

Other useful make targets:

```sh
make configure
make build
make run
make clean
make cleanall
```

### Build output

The binary is created at:

```text
build/calm_ideas_wall
```

---

## Running manually

From the project root:

```sh
make run
```

or directly:

```sh
./build/calm_ideas_wall
```

If no configuration is provided, the app listens on:

```text
0.0.0.0:8000
```

You can change the port with the `PORT` environment variable:

```sh
PORT=8080 ./build/calm_ideas_wall
```

You can also change it from the command line:

```sh
./build/calm_ideas_wall --port 8080
```

For local development:

```sh
PORT=8000 make run
```

or:

```sh
./build/calm_ideas_wall --host 127.0.0.1 --port 8000
```

---

## Where are logs stored?

The application writes a log file named:

```text
calm_ideas_wall.log
```

in the **current working directory**.

So if you run:

```sh
./build/calm_ideas_wall
```

from the project root, the log is written to:

```text
./calm_ideas_wall.log
```

If you run it from a different directory, the log file is created in that directory.

For a systemd or OpenRC deployment, the working directory is fixed, so the log location becomes predictable.

In the production examples below, the log file is expected at:

```text
/opt/calm_ideas_wall/calm_ideas_wall.log
```

---

## Production deployment model

This project is best run as a **single-user local service** behind nginx.

Recommended production setup:

- dedicated system user: `calm_ideas_wall`
- app directory: `/opt/calm_ideas_wall`
- local bind: `127.0.0.1:8080`
- nginx reverse proxy handling public traffic
- config file: `/opt/calm_ideas_wall/config`
- logs written to `/opt/calm_ideas_wall/calm_ideas_wall.log`

### Recommended production config file

Create `/opt/calm_ideas_wall/config` with contents like:

```text
host: 127.0.0.1
port: 8080
max_projects: 10
max_growing: 3
```

With this config, you usually do **not** need to pass `--host` or `--port` in the service definition.

---

## Deploy to `/opt/calm_ideas_wall`

This assumes you want the final runtime directory to be:

```text
/opt/calm_ideas_wall
```

### 1. Create a dedicated user

```sh
sudo useradd --system --home-dir /opt/calm_ideas_wall --shell /usr/sbin/nologin calm_ideas_wall
```

### 2. Create the app directory

```sh
sudo mkdir -p /opt/calm_ideas_wall
sudo chown calm_ideas_wall:calm_ideas_wall /opt/calm_ideas_wall
```

### 3. Copy or build the project into `/opt/calm_ideas_wall`

For example, if your source checkout is at `/home/you/calm_ideas_wall_cpp`:

```sh
sudo cp -a /home/you/calm_ideas_wall_cpp/. /opt/calm_ideas_wall/
```

Then make sure the owner is correct:

```sh
sudo chown -R calm_ideas_wall:calm_ideas_wall /opt/calm_ideas_wall
```

### 4. Create the config file in the production directory

For example:

```sh
sudo tee /opt/calm_ideas_wall/config <<'EOF'
host: 127.0.0.1
port: 8080
max_projects: 10
max_growing: 3
EOF
```

Then set ownership:

```sh
sudo chown calm_ideas_wall:calm_ideas_wall /opt/calm_ideas_wall/config
```

### 5. Build in the production directory

```sh
cd /opt/calm_ideas_wall
sudo -u calm_ideas_wall make build
```

After this step, the binary should exist at:

```text
/opt/calm_ideas_wall/build/calm_ideas_wall
```

If you prefer, you can copy the final binary to:

```sh
sudo cp /opt/calm_ideas_wall/build/calm_ideas_wall /opt/calm_ideas_wall/calm_ideas_wall
sudo chown calm_ideas_wall:calm_ideas_wall /opt/calm_ideas_wall/calm_ideas_wall
sudo chmod 0755 /opt/calm_ideas_wall/calm_ideas_wall
```

For the service files below, I am using:

```text
/opt/calm_ideas_wall/calm_ideas_wall
```

---

## systemd service file

Install this as:

```text
/etc/systemd/system/calm-idea-wall.service
```

### Enable and start

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now calm-idea-wall
```

### Check status

```sh
sudo systemctl status calm-idea-wall
```

### Watch logs from the app log file

```sh
tail -f /opt/calm_ideas_wall/calm_ideas_wall.log
```

If your service file sets:

```ini
WorkingDirectory=/opt/calm_ideas_wall
```

then the default config file used will be:

```text
/opt/calm_ideas_wall/config
```

---

## OpenRC service file

Install this as:

```text
/etc/init.d/calm_ideas_wall
```

Make it executable:

```sh
sudo cp deploy/calm_ideas_wall.openrc /etc/init.d/calm_ideas_wall
sudo chmod 0755 /etc/init.d/calm_ideas_wall
```

Enable and start:

```sh
sudo rc-update add calm_ideas_wall default
sudo rc-service calm_ideas_wall start
```

Check status:

```sh
rc-service calm_ideas_wall status
```

---

## nginx reverse proxy example

Example server block:

```nginx
server {
    listen 80;
    server_name ideas.example.com;

    root /opt/calm_ideas_web;
    index index.html;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;

        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        proxy_read_timeout 60s;
        proxy_send_timeout 60s;
    }
}
```

If you use HTTPS, a typical TLS setup looks like this:

```nginx
server {
    listen 443 ssl;
    http2 on;
    server_name ideas.example.com;

    ssl_certificate /etc/letsencrypt/live/ideas.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/ideas.example.com/privkey.pem;

    root /opt/calm_ideas_web;
    index index.html;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;

        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        proxy_read_timeout 60s;
        proxy_send_timeout 60s;
    }
}

server {
    listen 80;
    server_name ideas.example.com;
    return 301 https://$host$request_uri;
}
```

After editing nginx configuration:

```sh
sudo nginx -t
sudo systemctl reload nginx
```

or, on OpenRC:

```sh
sudo nginx -t
sudo rc-service nginx reload
```

---

## Recommended production notes

### 1. Prefer a local-only backend

For a reverse-proxied production setup, the cleanest design is:

```text
nginx (public) -> 127.0.0.1:8080 -> calm_ideas_wall
```

You can enforce this in the config file:

```text
host: 127.0.0.1
port: 8080
```

### 2. Keep the app behind nginx

Do not expose the C++ service directly if possible. nginx should handle:

- TLS
- public hostname
- basic request proxying

### 3. Backup the data file

The main state file is:

```text
/opt/calm_ideas_wall/projects.jsonl
```

Back this up regularly.

You may also want to back up:

```text
/opt/calm_ideas_wall/config
```

---

## License

MIT License

Copyright (c) 2026 Fabio Balzano

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
