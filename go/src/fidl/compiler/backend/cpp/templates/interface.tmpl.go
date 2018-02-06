// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Interface = `
{{- define "Params" -}}
  {{- range $index, $param := . -}}
    {{if $index}}, {{end}}{{$param.Name|$param.Type.Decorate}}
  {{- end -}}
{{ end }}

{{- define "ParamTypes" -}}
  {{- range $index, $param := . -}}
    {{if $index}}, {{end}}{{""|$param.Type.Decorate}}
  {{- end -}}
{{ end }}

{{- define "RequestMethodSignature" -}}
  {{- if .HasResponse -}}
{{ .Name }}({{ template "Params" .Request }}{{ if .Request }}, {{ end }}{{ .CallbackType }} callback)
  {{- else -}}
{{ .Name }}({{ template "Params" .Request }})
  {{- end -}}
{{ end -}}

{{- define "InterfaceDeclaration" -}}
class {{ .ProxyName }};
class {{ .StubName }};

class {{ .Name }} {
 public:
  using Proxy_ = {{ .ProxyName }};
  using Stub_ = {{ .StubName }};
  virtual ~{{ .Name }}();

  {{- range $method := .Methods }}
    {{- if $method.HasRequest }}
      {{- if $method.HasResponse }}
  using {{ $method.CallbackType }} =
      std::function<void({{ template "Params" .Response }})>;
      {{- end }}
  virtual void {{ template "RequestMethodSignature" . }} = 0;
    {{- end }}
  {{- end }}
};

using {{ .Name }}Ptr = ::fidl::InterfacePtr<{{ .Name }}>;

class {{ .ProxyName }} : public {{ .Name }} {
 public:
  explicit {{ .ProxyName }}(::fidl::internal::ProxyController* controller);
  ~{{ .ProxyName }}() override;

  {{- range $method := .Methods }}
    {{- if $method.HasRequest }}
  void {{ template "RequestMethodSignature" . }} override;
    {{- end }}
{{- end }}

 private:
  {{ .ProxyName }}(const {{ .ProxyName }}&) = delete;
  {{ .ProxyName }}& operator=(const {{ .ProxyName }}&) = delete;

  ::fidl::internal::ProxyController* controller_;
};

class {{ .StubName }} : public ::fidl::internal::Stub {
 public:
  explicit {{ .StubName }}({{ .Name }}* impl);
  ~{{ .StubName }}() override;

  zx_status_t Dispatch(::fidl::Message message,
                       ::fidl::internal::PendingResponse response) override;

 private:
  {{ .Name }}* impl_;
};
{{end}}

{{- define "InterfaceDefinition" -}}
namespace {
{{ range $method := .Methods }}
  {{- if $method.HasRequest }}
constexpr uint32_t {{ .OrdinalName }} = {{ $method.Ordinal }}u;
  {{- end }}
{{- end }}

}  // namespace

{{ .Name }}::~{{ .Name }}() = default;

{{ .ProxyName }}::{{ .ProxyName }}(::fidl::internal::ProxyController* controller)
    : controller_(controller) {}

{{ .ProxyName }}::~{{ .ProxyName }}() = default;

{{ range $method := .Methods }}
  {{- if $method.HasRequest }}
void {{ $.ProxyName }}::{{ template "RequestMethodSignature" . }} {
  // TODO(abarth): Implement method.
  (void) controller_;
}
  {{- end }}
{{- end }}

{{ .StubName }}::{{ .StubName }}({{ .Name }}* impl) : impl_(impl) {}

{{ .StubName }}::~{{ .StubName }}() = default;

zx_status_t {{ .StubName }}::Dispatch(
    ::fidl::Message message,
    ::fidl::internal::PendingResponse response) {
  zx_status_t status = ZX_OK;
  switch (message.ordinal()) {
    {{- range $method := .Methods }}
      {{- if $method.HasRequest }}
    case {{ .OrdinalName }}: {
      // TODO(abarth): Dispatch method.
      (void) impl_;
      break;
    }
      {{- end }}
    {{- end }}
    default: {
      status = ZX_ERR_NOT_SUPPORTED;
      break;
    }
  }
  return status;
}
{{- end }}
`
