---
schema_version: 1
kind: task
id: 123e4567-e89b-42d3-a456-426614174001
title: Reference task
status: todo
priority: normal
tags: [reference]
due: "2026-09-07"
recurrence:
  enabled: true
  mode: fixed_calendar
  unit: weeks
  interval: 1
  weekdays: [1, 3]
  x-mobile: {timezone: UTC}
reminders:
  - id: reminder-1
    minutes_before: 60
    x-mobile: {sound: bell}
x-mobile:
  nested: [1, true, {value: preserved}]
---
# Notes

[Attachment](assets/reference.bin)  

<div data-custom="yes">Source-only markup</div>

[Unicode attachment](assets/notes%20%C3%A9.txt)
