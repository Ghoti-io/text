# Shared by every tools/conformance/run*.sh script.
#
# Sourced, not executed: it sets `archive` and `generated` in the caller.
#
# It exists because the three lines below were written ten times across seven
# scripts, and all ten had the same two faults.
#
#   archive=$(ls "$root"/build/*/release/apps/*.a 2>/dev/null | head -1)
#   [ -n "$archive" ] || { echo "build the library first (make)" >&2; exit 1; }
#   generated=$(dirname "$(dirname "$archive")")/generated
#
# **It took whatever archive was there.** The absent case was handled and the
# *stale* one was not, and no conformance target named the library as a
# prerequisite, so `make conformance-toml-next` straight after editing src/
# linked the previous build and printed a score about code that was no longer
# there. That is not a hypothetical: two mutation runs on 2026-09-28 both
# reported the unmutated figure, and a planted defect looked undetectable until
# the archive was rebuilt by hand. Same family as the stale-artifact lesson -
# verify the artifact, not the command.
#
# **And it hardcoded `release`.** `make conformance BUILD=debug` scored the
# release archive, or failed if there was none, whatever the caller asked for.
#
# The Makefile knows where it just put the library, so it passes it, the way it
# already passes PREFIX and DEP_PCS. A glob is what let the wrong answer
# through, so there is no glob and no fallback: a script run by hand says what
# to set.
conformance_library() {
	if [ -z "$ARCHIVE" ]; then
		echo "ARCHIVE must be set; the Makefile passes it" >&2
		echo "  (CONFORMANCE_ENV there; by hand, ARCHIVE=build/<platform>/release/apps/<lib>.a)" >&2
		return 1
	fi
	if [ ! -f "$ARCHIVE" ]; then
		echo "$ARCHIVE is not there; build the library first (make)" >&2
		return 1
	fi
	archive=$ARCHIVE
	# build/<platform>/<build>/apps/<lib>.a -> build/<platform>/<build>/generated.
	# Derived from the archive rather than from a second variable, so the two
	# cannot disagree about which build tree this is.
	generated=$(dirname "$(dirname "$archive")")/generated
	if [ ! -d "$generated" ]; then
		echo "$generated is not there; the generated headers go with the archive" >&2
		return 1
	fi
}
