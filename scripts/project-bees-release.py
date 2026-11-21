"""Plan a version bump, or publish an approved plan after CI."""

import argparse
import re
import subprocess
import sys


MAIN = "refs/heads/main"
ORIGIN_MAIN = "refs/remotes/origin/main"
VERSION = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
BUMP_COMPONENT = {"major": 0, "minor": 1, "patch": 2}


def git(*args):
    return subprocess.check_output(("git", *args), text=True).strip()


def bump_tag(version, bump):
    component = BUMP_COMPONENT[bump]
    next_version = version[:component] + (version[component] + 1,) + (0,) * (2 - component)
    return "v" + ".".join(map(str, next_version))


def plan_release(source, bump, marker):
    git("fetch", "--prune", "--prune-tags", "origin", f"{MAIN}:{ORIGIN_MAIN}")
    if source != git("rev-parse", "HEAD") or source != git("rev-parse", ORIGIN_MAIN):
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
    if subprocess.run(("git", "merge-base", "--is-ancestor", latest_commit, source)).returncode:
        raise ValueError(f"Highest stable tag {latest} is outside origin/main history")

    tags_at_source = git("tag", "--points-at", source).splitlines()
    if any(VERSION.fullmatch(tag) for tag in tags_at_source):
        if latest_commit == source and (
            git("for-each-ref", "--format=%(objecttype) %(contents:subject)", f"refs/tags/{latest}")
            == f"tag {marker}"
        ):
            print(f"Resuming {latest} at {source}", file=sys.stderr)
            return latest, True
        raise ValueError(f"Source {source} already has a stable version tag")

    tag = bump_tag(version, bump)
    print(f"{latest} -> {tag} at {source}", file=sys.stderr)
    return tag, False


def remote_ref(ref):
    return git("ls-remote", "origin", ref).partition("\t")[0]


def publish_release(source, tag, marker, existing_tag, repo):
    # Include drafts in the lookup so an existing release is never replaced.
    existing_release = subprocess.check_output(
        (
            "gh", "api", "--paginate", f"repos/{repo}/releases?per_page=100",
            "--jq", f'.[] | select(.tag_name == "{tag}") | [.html_url, .draft, .prerelease] | @tsv',
        ),
        text=True,
    ).strip()
    tag_ref = f"refs/tags/{tag}"
    if not existing_tag:
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
    expected_refs = {f"{git('rev-parse', tag_ref)}\t{tag_ref}", f"{source}\t{peeled_ref}"}
    if set(git("ls-remote", "origin", tag_ref, peeled_ref).splitlines()) != expected_refs:
        raise ValueError(f"Remote tag {tag} does not match the tested source")

    if existing_release:
        url, draft, prerelease = existing_release.split("\t")
        if draft != "false" or prerelease != "false":
            raise ValueError(f"{tag} already has a draft or prerelease")
        print(f"{tag} already published at {url}", file=sys.stderr)
        return

    result = subprocess.check_output(
        ("gh", "release", "create", tag, "--repo", repo, "--verify-tag",
         "--title", f"project-bees {tag}", "--generate-notes"),
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
    args = parser.parse_args()
    if args.publish_tag is not None and not args.repo:
        parser.error("--publish-tag requires --repo")

    marker = f"Project Bees release workflow run {args.run_id}"
    try:
        tag, existing_tag = plan_release(args.source, args.bump, marker)
        if args.publish_tag is not None:
            if args.publish_tag != tag:
                raise ValueError(f"Planned tag changed from {args.publish_tag} to {tag}")
            publish_release(args.source, tag, marker, existing_tag, args.repo)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release stopped: {error}\n")
    print(tag)


if __name__ == "__main__":
    main()
