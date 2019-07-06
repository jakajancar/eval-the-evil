# Eval the Evil

[![Releases](https://badgen.net/github/release/jakajancar/eval-the-evil/stable)](https://github.com/jakajancar/eval-the-evil/releases)
[![CircleCI](https://badgen.net/circleci/github/jakajancar/eval-the-evil)](https://circleci.com/gh/jakajancar/eval-the-evil)
![License](https://badgen.net/github/license/jakajancar/eval-the-evil)

## Developing

 1. Install Docker Compose (or e.g. Docker Desktop)

 2. (Re-)build and enter the development environment:

        docker-compose build && docker-compose run --rm dev-env

 3. Inside the container:
      - `bin/build` to build, binary will be placed into `build/`
      - `bin/test` to run tests
