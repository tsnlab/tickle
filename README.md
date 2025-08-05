# Template repository for Github CI

## Usage

Check [.github/workflows/](.github/workflows/)
It is for basic python+Rust project. Modify by yourself as needed.

Remove some lint-*.yml if you not need on your project

Check [.gitignore](.gitignore) file.
It is also basic gitignore file for C + Python project.
Check [This repo][gitignore] to find gitignore for another type

[gitignore]: https://github.com/github/gitignore


## Run locally

Run these commands on your demand

```sh
cargo check
cargo fmt
flake8
pyright
```


## Editor integration

### Vim

Use [ALE](https://github.com/dense-analysis/ale)

### VSCode

It'll automatically detects its settings
