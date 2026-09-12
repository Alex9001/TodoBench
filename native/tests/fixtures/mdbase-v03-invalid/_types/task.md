---
kind: mdbase.type
name: task
version: 1
match:
  path_glob: "tasks/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id, title]
    additionalProperties: true
    properties:
      id: { type: string }
      title: { type: string }
      status: { type: string, enum: [todo, done], default: todo }
      priority: { type: string, enum: [low, normal, high], default: normal }
      project_link: { type: string }
collection:
  display: { name_field: title }
  unique: [{ field: id, scope: type }]
  links:
    project_link:
      target_type: project
      validate_exists: true
  read_defaults:
    status: todo
    priority: normal
---
