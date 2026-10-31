#!/bin/bash
# repos.sh: work across x86Code's submodules (every one is a fork in the x86code GitHub org).
#   repos status                     dirty / unpushed submodules, with their branch
#   repos commit -m MSG [--dry-run]  commit tracked changes in every dirty submodule (deepest first) with MSG, push
#                                    each to its branch, then commit and push the bumped pointers in x86code
#   repos upstream-sync [--apply] [PATH...]
#                                    for each fork with an x86code branch: fetch the GitHub parent's default branch
#                                    and rebase x86code onto it (dry run: report behind/ahead and conflicts only).
#                                    --apply rebases clean trees and pushes with --force-with-lease; a conflicting
#                                    rebase is aborted and reported. Forks without an x86code branch get GitHub's
#                                    "Sync fork" (merge-upstream) on their default branch.
# Untracked files are never added; they are listed so they can be added (or ignored) by hand.
set -u
X=${X86SIM_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)}
cd "$X" || exit 1

# submodule paths, deepest first (reverse of foreach's pre-order: children come after their parents)
subs() { git submodule foreach --recursive -q 'echo "$displaypath"' | tac; }

dirty() { [ -n "$(git -C "$1" status --porcelain --untracked-files=no)" ]; }
unpushed() {
	local b; b=$(git -C "$1" symbolic-ref -q --short HEAD) || return 1
	[ -n "$(git -C "$1" log --oneline "origin/$b..$b" 2>/dev/null | head -1)" ]
}
untracked() { git -C "$1" ls-files --others --exclude-standard | head -5; }

cmd_status() {
	local p b n=0
	while read -r p; do
		b=$(git -C "$p" symbolic-ref -q --short HEAD || echo "(detached $(git -C "$p" rev-parse --short HEAD))")
		if dirty "$p" || unpushed "$p"; then
			n=$((n+1))
			printf '%-55s %-22s %s%s\n' "$p" "$b" "$(dirty "$p" && echo "changes ")" "$(unpushed "$p" && echo unpushed)"
		fi
		[ -n "$(untracked "$p")" ] && printf '%-55s untracked: %s\n' "$p" "$(untracked "$p" | tr '\n' ' ')"
	done < <(subs)
	[ $n = 0 ] && echo "all submodules clean and pushed"
	git status --short --ignore-submodules=all --untracked-files=no | sed 's/^/x86code: /'
}

cmd_commit() {
	local msg="" dry=0 p b
	while [ $# -gt 0 ]; do
		case $1 in
			-m) msg=$2; shift ;;
			--dry-run|-n) dry=1 ;;
			*) echo "repos commit: unknown argument $1" >&2; return 2 ;;
		esac
		shift
	done
	[ -n "$msg" ] || { echo "repos commit: -m MSG is required" >&2; return 2; }
	while read -r p; do
		dirty "$p" || unpushed "$p" || continue
		b=$(git -C "$p" symbolic-ref -q --short HEAD) || {
			echo "$p: has changes but is on a detached HEAD; check out its x86code branch first" >&2; return 1; }
		[ -n "$(untracked "$p")" ] && echo "$p: not adding untracked: $(untracked "$p" | tr '\n' ' ')"
		if [ $dry = 1 ]; then echo "would commit+push $p ($b)"; continue; fi
		if dirty "$p"; then git -C "$p" add -u && git -C "$p" commit -q -m "$msg" || return 1; fi
		git -C "$p" push -q origin "$b" || { echo "$p: push failed" >&2; return 1; }
		echo "$p: $(git -C "$p" log --oneline -1) -> origin/$b"
	done < <(subs)
	# x86code: only the submodule pointers (other changes in the main repo are left alone)
	local bumped; bumped=$(git diff --name-only --ignore-submodules=none | while read -r f; do
		[ "$(git ls-files -s -- "$f" | cut -c1-6)" = 160000 ] && echo "$f"; done)
	[ -z "$bumped" ] && { echo "x86code: no submodule pointers to bump"; return 0; }
	if [ $dry = 1 ]; then echo "would bump in x86code: $(echo $bumped)"; return 0; fi
	git add -- $bumped && git commit -q -m "$msg" -- $bumped && git push -q || return 1
	echo "x86code: $(git log --oneline -1)"
}

cmd_sync() {
	local apply=0 p url name parent pb behind ahead
	local -a only=()
	while [ $# -gt 0 ]; do
		case $1 in --apply) apply=1 ;; *) only+=("$1") ;; esac
		shift
	done
	while read -r p; do
		if [ ${#only[@]} -gt 0 ]; then printf '%s\n' "${only[@]}" | grep -qxF "$p" || continue; fi
		url=$(git -C "$p" remote get-url origin) || continue
		name=$(basename "$url" .git)
		parent=$(gh api "repos/x86code/$name" --jq '.parent.full_name // empty' 2>/dev/null)
		[ -n "$parent" ] || { echo "$p: x86code/$name has no GitHub parent; skipped"; continue; }
		if ! git -C "$p" show-ref -q --verify refs/heads/x86code && \
		   ! git -C "$p" ls-remote -q --exit-code origin refs/heads/x86code >/dev/null; then
			# unchanged fork: keep its default branch in sync on GitHub (the pinned commit is darling's choice)
			if [ $apply = 1 ]; then
				pb=$(gh api "repos/x86code/$name" --jq .default_branch)
				gh api -X POST "repos/x86code/$name/merge-upstream" -f branch="$pb" --jq "\"$p: \(.message)\"" 2>/dev/null \
					|| echo "$p: sync fork failed"
			fi
			continue
		fi
		pb=$(gh api "repos/$parent" --jq .default_branch)
		git -C "$p" remote get-url upstream >/dev/null 2>&1 || git -C "$p" remote add upstream "https://github.com/$parent.git"
		git -C "$p" fetch -q upstream "$pb" && git -C "$p" fetch -q origin x86code || { echo "$p: fetch failed"; continue; }
		behind=$(git -C "$p" rev-list --count "origin/x86code..upstream/$pb")
		ahead=$(git -C "$p" rev-list --count "upstream/$pb..origin/x86code")
		if [ "$behind" = 0 ]; then echo "$p: up to date with $parent:$pb ($ahead ours)"; continue; fi
		if [ $apply = 0 ]; then
			if git -C "$p" merge-tree --write-tree "origin/x86code" "upstream/$pb" >/dev/null 2>&1; then
				echo "$p: $behind behind $parent:$pb, $ahead ours, merges cleanly"
			else
				echo "$p: $behind behind $parent:$pb, $ahead ours, CONFLICTS:"
				git -C "$p" merge-tree --write-tree --name-only "origin/x86code" "upstream/$pb" 2>/dev/null | sed -n '2,/^$/{/^$/!s/^/    /p}'
			fi
			continue
		fi
		if [ "$(git -C "$p" symbolic-ref -q --short HEAD)" != x86code ] || dirty "$p"; then
			echo "$p: not on a clean x86code branch; skipped"; continue
		fi
		git -C "$p" merge -q --ff-only origin/x86code 2>/dev/null
		if git -C "$p" rebase -q "upstream/$pb" >/dev/null 2>&1; then
			git -C "$p" push -q --force-with-lease=x86code origin x86code && echo "$p: rebased onto $parent:$pb (+$behind) and pushed"
		else
			echo "$p: rebase onto $parent:$pb CONFLICTS (aborted):"
			git -C "$p" diff --name-only --diff-filter=U | sed 's/^/    /'
			git -C "$p" rebase --abort
		fi
	done < <(subs)
	[ $apply = 0 ] || echo "then: x86sim repos commit -m '...' to bump the pointers"
}

c=${1:-status}; shift || true
case $c in
	status) cmd_status ;;
	commit) cmd_commit "$@" ;;
	upstream-sync|sync) cmd_sync "$@" ;;
	*) sed -n '2,13p' "$0" >&2; exit 2 ;;
esac
