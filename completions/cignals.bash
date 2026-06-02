# shellcheck shell=bash

_cignals_load_completions() {
  mapfile -t COMPREPLY < <(compgen -W "$1" -- "$2")
}

_cignals_complete() {
  local cur prev words cword

  if declare -F _init_completion >/dev/null 2>&1; then
    _init_completion || return
  else
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD - 1]}"
    words=("${COMP_WORDS[@]}")
    cword=$COMP_CWORD
  fi

  local commands="start status quit stop get set"
  local top_level_opts="--daemon"
  local start_opts="--interval"
  local subcommand="${words[1]:-}"

  case "$cword" in
    1)
      _cignals_load_completions "$commands $top_level_opts" "$cur"
      return 0
      ;;
  esac

  case "$subcommand" in
    start|--daemon)
      if [[ $cword -eq 2 ]]; then
        _cignals_load_completions "$start_opts" "$cur"
      fi
      return 0
      ;;
    get|set)
      if [[ $cword -eq 2 ]]; then
        _cignals_load_completions "interval" "$cur"
      fi
      return 0
      ;;
    quit|stop|status)
      return 0
      ;;
  esac

  _cignals_load_completions "$commands $top_level_opts" "$cur"
}

complete -F _cignals_complete cignals ./cignals
