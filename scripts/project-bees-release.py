"""Plan a version bump, or publish an approved plan after CI."""

import argparse
import re
import subprocess
import sys


MAIN = "refs/heads/main"
ORIGIN_MAIN = "refs/remotes/origin/main"
VERSION = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
BUMP_COMPONENT = {"major": 0, "minor": 1, "patch": 2}
RUN_MARKER = "Project Bees release workflow run "


def git(*args):
    return subprocess.check_output(("git", *args), text=True).strip()


def bump_tag(version, bump):
    component = BUMP_COMPONENT[bump]
    next_version = version[:component] + (version[component] + 1,) + (0,) * (2 - component)
    return "v" + ".".join(map(str, next_version))


def is_ancestor(source, tip):
    return subprocess.run(("git", "merge-base", "--is-ancestor", source, tip)).returncode == 0


def plan_release(source, bump):
    if source != git("rev-parse", ORIGIN_MAIN):
        raise ValueError("The dispatch commit is no longer the checked-out tip of origin/main")

    tags = {
        tuple(map(int, match.groups())): tag
        for tag in git("tag", "--list").splitlines()
        if (match := VERSION.fullmatch(tag))
    }
    if not tags:
        raise ValueError("No stable vMAJOR.MINOR.PATCH tags found")

    version = max(tags)
    latest = tags[version]
    latest_commit = git("rev-parse", f"refs/tags/{latest}^{{commit}}")
    if not is_ancestor(latest_commit, source):
        raise ValueError(f"Highest stable tag {latest} is outside origin/main history")

    tags_at_source = git("tag", "--points-at", source).splitlines()
    if any(VERSION.fullmatch(tag) for tag in tags_at_source):
        raise ValueError(f"Source {source} already has a stable version tag")

    tag = bump_tag(version, bump)
    print(f"{latest} -> {tag} at {source}", file=sys.stderr)
    return tag


def remote_ref(ref):
    return git("ls-remote", "origin", ref).partition("\t")[0]


def workflow_tags():
    tags = git("for-each-ref", "--format=%(refname:strip=2)%00%(objecttype)%00%(contents:subject)", "refs/tags")
    return {
        tag: subject
        for tag, kind, subject in (line.split("\0", 2) for line in tags.splitlines())
        if kind == "tag" and VERSION.fullmatch(tag) and re.fullmatch(RUN_MARKER + r"[0-9]+", subject)
    }


def publish_release(source, tag, marker, existing_tag, repo, managed_tags):
    release_rows = subprocess.check_output(
        (
            "gh", "api", "--paginate", f"repos/{repo}/releases?per_page=100",
            "--jq", ".[] | [.tag_name, .html_url, .draft, .prerelease] | @tsv",
        ),
        text=True,
    )
    releases = {
        name: (url, draft, prerelease)
        for name, url, draft, prerelease in (line.split("\t") for line in release_rows.splitlines())
    }
    existing_release = releases.get(tag)
    tag_ref = f"refs/tags/{tag}"
    if not existing_tag:
        for name, owner in managed_tags.items():
            release = releases.get(name)
            if not release or release[1:] != ("false", "false"):
                raise ValueError(f"Unfinished release {name}; rerun failed jobs for Actions run {owner.removeprefix(RUN_MARKER)}")
        if existing_release or remote_ref(tag_ref):
            raise ValueError(f"{tag} already exists on GitHub")
        git(
            "-c", "user.name=github-actions[bot]",
            "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com",
            "tag", "-a", tag, source, "-m", marker,
        )
        if source != remote_ref(MAIN):
            raise ValueError("origin/main moved after CI; start a new release run")
        git("push", "--no-follow-tags", "origin", f"{tag_ref}:{tag_ref}")

    # Compare the exact tag object and the peeled commit on GitHub before publishing.
    peeled_ref = f"{tag_ref}^{{}}"
    if existing_tag:
        git("fetch", "--no-tags", "origin", f"{MAIN}:{ORIGIN_MAIN}")
    refs = {ref: oid for oid, ref in
            (line.split("\t") for line in git("ls-remote", "origin", MAIN, tag_ref, peeled_ref).splitlines())}
    if refs.get(tag_ref) != git("rev-parse", tag_ref) or refs.get(peeled_ref) != source:
        raise ValueError(f"Remote tag {tag} does not match the tested source")
    tip = refs.get(MAIN)
    if existing_tag and (not tip or not is_ancestor(source, tip)):
        raise ValueError("The tagged source is outside origin/main history")

    if existing_release:
        url, draft, prerelease = existing_release
        if draft != "false" or prerelease != "false":
            raise ValueError(f"{tag} already has a draft or prerelease")
        print(f"{tag} already published at {url}", file=sys.stderr)
        return

    if not existing_tag and source != tip:
        raise ValueError("origin/main moved before the GitHub Release could be created")
    result = subprocess.check_output(
        ("gh", "release", "create", tag, "--repo", repo, "--verify-tag",
         "--target", source, "--title", f"project-bees {tag}", "--generate-notes"),
        text=True,
    )
    print(result.strip(), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bump", choices=BUMP_COMPONENT)
    parser.add_argument("source", help="dispatch commit SHA")
    parser.add_argument("run_id", help="GitHub Actions run ID, which stays the same on retries")
    parser.add_argument("--publish-tag", help="publish this exact version after CI")
    parser.add_argument("--repo", help="GitHub owner/repository for publication")
    parser.add_argument("--recover", action="store_true", help="finish the original run's existing tag")
    args = parser.parse_args()
    if args.publish_tag is not None and not args.repo:
        parser.error("--publish-tag requires --repo")
    if args.recover and args.publish_tag is None:
        parser.error("--recover requires --publish-tag")

    marker = f"{RUN_MARKER}{args.run_id}"
    try:
        git("fetch", "--prune", "--prune-tags", "origin", f"{MAIN}:{ORIGIN_MAIN}")
        if args.source != git("rev-parse", "HEAD"):
            raise ValueError("Checkout does not match the dispatch commit")
        tag = args.publish_tag
        managed_tags = workflow_tags() if tag is not None else {}
        existing_tag = tag in managed_tags
        if args.recover and not existing_tag:
            raise ValueError(f"Recovery requires an existing workflow tag for {tag}")
        if existing_tag:
            if managed_tags[tag] != marker:
                raise ValueError(f"{tag} belongs to a different release workflow run")
            if not args.recover:
                raise ValueError(f"{tag} already exists; rerun failed jobs to recover it")
            if args.source != git("rev-parse", f"refs/tags/{tag}^{{commit}}"):
                raise ValueError(f"{tag} does not match the dispatch commit")
            if not is_ancestor(args.source, ORIGIN_MAIN):
                raise ValueError("The tagged source is outside origin/main history")
        else:
            planned = plan_release(args.source, args.bump)
            if tag is not None and planned != tag:
                raise ValueError(f"Planned tag changed from {tag} to {planned}")
            tag = planned
        if args.publish_tag is not None:
            publish_release(args.source, tag, marker, existing_tag, args.repo, managed_tags)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release stopped: {error}\n")
    print(tag)


if __name__ == "__main__":
    main()
