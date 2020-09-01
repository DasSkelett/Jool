---
language: en
layout: default
category: Home
title: Home
---

# Home

-------------------

## Introduction

Jool is an Open Source [SIIT and NAT64](intro-xlat.html) for Linux.

* [Click here](documentation.html) to start getting acquainted with the software.
* [Click here](download.html) to download Jool.

-------------------

## Status

As far as we know, Jool is a [compliant](intro-jool.html#compliance) SIIT and Stateful NAT64.

Its latest version is [4.1.3](download.html#41x) and its most mature version is [4.0.9](download.html#40x).

-------------------

## Survey

<iframe src="https://docs.google.com/forms/d/e/1FAIpQLSe_9_wBttFGd9aJ7lKXiJvIN7wWZm_C6yy3gU0Ttepha275nQ/viewform?embedded=true" width="640" height="300" frameborder="0" marginheight="0" marginwidth="0">Loading…</iframe>

-------------------

## Latest News

### 2020-09-02

[Jool 4.1.3](download.html) has been released.

It only has one bugfix, though it's fairly critical:

1. [#338](https://github.com/NICMx/Jool/issues/338), [#339](https://github.com/NICMx/Jool/issues/339): Patch SIIT Jool's generic blacklist. SIIT Jool should no longer steal traffic intended for its own node.

> Remember that `lowest-ipv6-mtu`'s paranoid default might induce unnecessary fragmentation. If you want Jool 4.1 to reach 4.0's performance, please review the [MTU documentation](mtu.html).

