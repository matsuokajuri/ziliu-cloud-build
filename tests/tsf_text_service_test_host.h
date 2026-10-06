#pragma once
// COM host doubles for the actual TextService regression executable. Derived
// from the existing privacy fixture; synthetic text only, no TSF registration.
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../src/tsf/src/input_privacy.h"
namespace ziliu::tsf::test {
inline std::function<void(std::string_view, void*)> host_callback;
inline void Event(std::string_view name, void* object) {
  auto callback = host_callback;
  if (callback) callback(name, object);
}
inline void Expect(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
class Compartment final : public ITfCompartment {
 public:
  VARTYPE type = VT_EMPTY;
  LONG value = 0;
  HRESULT result = S_OK;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfCompartment) return E_NOINTERFACE;
    *out = static_cast<ITfCompartment*>(this);
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP SetValue(TfClientId, const VARIANT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetValue(VARIANT* out) override {
    out->vt = type;
    out->lVal = value;
    return result;
  }
};

class Range final : public ITfRange {
 public:
  inline static LONG active_clones = 0;
  inline static HRESULT equal_start_result = S_OK;
  inline static HRESULT equal_end_result = S_OK;
  inline static TfAnchor last_equal_start_anchor = TF_ANCHOR_START;
  inline static TfAnchor last_equal_end_anchor = TF_ANCHOR_END;
  int writes = 0;
  bool collapsed = false;
  bool empty = false;
  bool heap_allocated = false;
  TfAnchor anchor = TF_ANCHOR_END;
  HRESULT collapse_result = S_OK;
  HRESULT clone_result = S_OK;
  int collapse_calls = 0;
  int clone_calls = 0;
  int equal_start_calls = 0;
  int equal_end_calls = 0;
  int special_collapse_call = 0;
  HRESULT special_collapse_result = S_OK;
  HRESULT empty_result = S_OK;
  HRESULT shift_result = S_OK;
  HRESULT text_result = S_OK;
  std::wstring text;
  ULONG reported_text_count = 0;
  ULONG requested_text_count = 0;
  DWORD text_flags = 0;
  int get_text_calls = 0;
  LONG start_position = 0;
  LONG end_position = 0;
  bool allow_text_read = false;
  void (*after_get_text)(void*) = nullptr;
  void* after_get_text_context = nullptr;
  LONG shifted_amount = 0;
  LONG reported_shifted = -999;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfRange) return E_NOINTERFACE;
    *out = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references;
    if (remaining == 0 && heap_allocated) {
      --active_clones;
      delete this;
    }
    return remaining;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor selected) override {
    Event("Range.Collapse", this);
    ++collapse_calls;
    anchor = selected;
    const HRESULT current_result =
        collapse_calls == special_collapse_call ? special_collapse_result : collapse_result;
    collapsed = SUCCEEDED(current_result);
    return current_result;
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* buffer, ULONG requested,
                       ULONG* count) override {
    Event("Range.GetText", this);
    ++get_text_calls;
    Expect(allow_text_read, "field text is read only by an explicitly opted-in synthetic fixture");
    text_flags = flags;
    requested_text_count = requested;
    const ULONG copied = static_cast<ULONG>(std::min<std::size_t>(text.size(), requested));
    if (copied != 0) std::copy_n(text.data(), copied, buffer);
    *count = reported_text_count == 0 ? copied : reported_text_count;
    if (after_get_text != nullptr) after_get_text(after_get_text_context);
    return text_result;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* value, LONG n) override {
    Event("Range.SetText", this);
    text.assign(value, static_cast<std::size_t>(n));
    ++writes;
    return S_OK;
  }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
    shifted_amount = count;
    if (shifted != nullptr) *shifted = reported_shifted == -999 ? count : reported_shifted;
    return shift_result;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* result) override {
    *result = empty ? TRUE : FALSE;
    return empty_result;
  }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange* other, TfAnchor selected_anchor,
                            BOOL* same) override {
    Event("Range.IsEqualStart", this);
    ++equal_start_calls;
    last_equal_start_anchor = selected_anchor;
    if (FAILED(equal_start_result)) return equal_start_result;
    const auto* other_range = static_cast<const Range*>(other);
    *same = start_position == other_range->start_position ? TRUE : FALSE;
    return equal_start_result;
  }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange* other, TfAnchor selected_anchor,
                          BOOL* same) override {
    Event("Range.IsEqualEnd", this);
    ++equal_end_calls;
    last_equal_end_anchor = selected_anchor;
    if (FAILED(equal_end_result)) return equal_end_result;
    const auto* other_range = static_cast<const Range*>(other);
    *same = end_position == other_range->end_position ? TRUE : FALSE;
    return equal_end_result;
  }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange* other, TfAnchor selected,
                            LONG* result) override {
    Event("Range.CompareStart", this);
    const auto* target = static_cast<const Range*>(other);
    *result = start_position -
              (selected == TF_ANCHOR_START ? target->start_position : target->end_position);
    return S_OK;
  }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange* other, TfAnchor selected, LONG* result) override {
    Event("Range.CompareEnd", this);
    const auto* target = static_cast<const Range*>(other);
    *result = end_position -
              (selected == TF_ANCHOR_START ? target->start_position : target->end_position);
    return S_OK;
  }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange** out) override {
    Event("Range.Clone", this);
    ++clone_calls;
    *out = nullptr;
    if (clone_result != S_OK) return clone_result;
    auto* copy = new (std::nothrow) Range();
    if (copy == nullptr) return E_OUTOFMEMORY;
    copy->heap_allocated = true;
    copy->start_position = start_position;
    copy->end_position = end_position;
    ++active_clones;
    *out = copy;
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class Property final : public ITfReadOnlyProperty {
 public:
  IUnknown* scope = nullptr;
  bool uniform_range = false;
  HRESULT result = S_OK;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfReadOnlyProperty) return E_NOINTERFACE;
    *out = static_cast<ITfReadOnlyProperty*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetType(GUID*) override { return E_NOTIMPL; }
  STDMETHODIMP EnumRanges(TfEditCookie, IEnumTfRanges**, ITfRange*) override { return E_NOTIMPL; }
  STDMETHODIMP GetValue(TfEditCookie, ITfRange* range, VARIANT* value) override {
    Event("Property.GetValue", this);
    if (!uniform_range && !static_cast<Range*>(range)->collapsed) {
      value->vt = VT_EMPTY;
      return S_FALSE;  // Simulate a selection containing multiple scope values.
    }
    value->vt = VT_UNKNOWN;
    value->punkVal = scope;
    if (scope != nullptr) scope->AddRef();
    return result;
  }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class Context : public ITfContext, public ITfCompartmentMgr {
 public:
  int selections = 0;
  Compartment disabled;
  Compartment empty;
  HRESULT property_result = S_FALSE;
  bool compartment_failure = false;
  bool property_present = false;
  bool selection_failure = false;
  HRESULT selection_result = S_OK;
  ULONG selection_fetched = 1;
  int selection_calls = 0;
  int special_selection_call = 0;
  HRESULT special_selection_result = S_OK;
  TfActiveSelEnd active_end = TF_AE_END;
  BOOL interim_character = FALSE;
  Range range;
  Property property;
  int nested_edit_requests = 0;
  HRESULT request_result = S_OK;
  bool defer_edit_session = false;
  bool foreign_thread_edit_session = false;
  bool repeat_edit_session = false;
  HRESULT secondary_session_result = E_PENDING;
  ITfEditSession* deferred_session = nullptr;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_ITfContext) *out = static_cast<ITfContext*>(this);
    if (id == IID_ITfCompartmentMgr) *out = static_cast<ITfCompartmentMgr*>(this);
    if (*out == nullptr) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetCompartment(REFGUID id, ITfCompartment** out) override {
    *out = nullptr;
    if (compartment_failure) return E_FAIL;
    if (id == GUID_COMPARTMENT_KEYBOARD_DISABLED) *out = &disabled;
    if (id == GUID_COMPARTMENT_EMPTYCONTEXT) *out = &empty;
    return *out != nullptr ? S_OK : E_INVALIDARG;
  }
  STDMETHODIMP ClearCompartment(TfClientId, REFGUID) override { return E_NOTIMPL; }
  STDMETHODIMP EnumCompartments(IEnumGUID**) override { return E_NOTIMPL; }
  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession* session, DWORD flags,
                                  HRESULT* result) override {
    ++nested_edit_requests;
    Expect((flags & TF_ES_SYNC) != 0, "synchronous synthetic edit");
    if (request_result != S_OK) {
      *result = E_UNEXPECTED;
      return request_result;
    }
    if (defer_edit_session) {
      deferred_session = session;
      deferred_session->AddRef();
      *result = E_PENDING;
      return S_OK;
    }
    if (foreign_thread_edit_session) {
      std::thread worker([&] { *result = session->DoEditSession(1); });
      worker.join();
      return S_OK;
    }
    *result = session->DoEditSession(1);
    if (repeat_edit_session) {
      secondary_session_result = session->DoEditSession(1);
      *result = secondary_session_result;
    }
    return S_OK;
  }
  STDMETHODIMP GetAppProperty(REFGUID id, ITfReadOnlyProperty** out) override {
    Expect(id == GUID_PROP_INPUTSCOPE, "only read input-scope metadata");
    *out = nullptr;
    if (property_present) {
      *out = &property;
      property.AddRef();
    }
    return property_result;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection,
                            ULONG* count) override {
    *count = 0;
    ++selection_calls;
    Event("Context.GetSelection", this);
    if (selection_failure) return E_FAIL;
    const HRESULT current_result =
        selection_calls == special_selection_call ? special_selection_result : selection_result;
    if (FAILED(current_result)) return current_result;
    selection->range = &range;
    selection->style.ase = active_end;
    selection->style.fInterimChar = interim_character;
    range.AddRef();
    *count = selection_fetched;
    return current_result;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override {
    Event("Context.SetSelection", this);
    ++selections;
    return S_OK;
  }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  STDMETHODIMP GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override {
    return E_NOTIMPL;
  }
};

class Scope final : public ITfInputScope {
 public:
  std::vector<InputScope> values;
  HRESULT result = S_OK;
  bool malformed = false;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != __uuidof(ITfInputScope)) return E_NOINTERFACE;
    *out = static_cast<ITfInputScope*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetInputScopes(InputScope** scopes, UINT* count) override {
    Event("Scope.GetInputScopes", this);
    *scopes = nullptr;
    *count = static_cast<UINT>(values.size());
    if (!values.empty() && !malformed) {
      *scopes = static_cast<InputScope*>(CoTaskMemAlloc(values.size() * sizeof(InputScope)));
      if (*scopes == nullptr) return E_OUTOFMEMORY;
      for (std::size_t i = 0; i < values.size(); ++i) (*scopes)[i] = values[i];
    }
    return result;
  }
  STDMETHODIMP GetPhrase(BSTR**, UINT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetRegularExpression(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSRGS(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetXML(BSTR*) override { return E_NOTIMPL; }
};

}  // namespace ziliu::tsf::test
