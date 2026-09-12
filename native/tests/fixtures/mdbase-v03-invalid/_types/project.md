---
kind: mdbase.type
name: project
version: 1
match:
  path_glob: "projects/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id, display_name]
    additionalProperties: true
    properties:
      id: { type: string }
      display_name: { type: string }
collection:
  display: { name_field: display_name }
  unique: [{ field: id, scope: type }]
---
