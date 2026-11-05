#include "Thermal.h"

#include <wbemidl.h>
#include <oleauto.h>

namespace tlr {

ThermalReader::ThermalReader() : services_(NULL) {}

ThermalReader::~ThermalReader() { release(); }

void ThermalReader::release()
{
    if (services_) { services_->Release(); services_ = NULL; }
}

bool ThermalReader::connect()
{
    IWbemLocator* locator = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&locator);
    if (FAILED(hr)) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = locator->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &services_);
    SysFreeString(ns);
    locator->Release();
    if (FAILED(hr)) { services_ = NULL; return false; }
    CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return true;
}

bool ThermalReader::read(int& tenthsC)
{
    if (!services_ && !connect()) return false;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    IEnumWbemClassObject* en = NULL;
    HRESULT hr = services_->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(query);
    SysFreeString(lang);
    if (FAILED(hr)) { release(); return false; }   // connect again at the next read
    bool found = false;
    int best = 0;
    for (;;) {
        IWbemClassObject* obj = NULL;
        ULONG got = 0;
        if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0) break;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &v, NULL, NULL)) && (v.vt == VT_I4 || v.vt == VT_UI4)) {
            int raw = v.vt == VT_I4 ? (int)v.lVal : (int)v.ulVal;   // tenths of a kelvin
            int t = raw - 2732;
            // A zone that reports 0 C or less, or above 150 C, is a placeholder, not a reading.
            if (t > 0 && t < 1500 && (!found || t > best)) { best = t; found = true; }
        }
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    if (found) tenthsC = best;
    return found;
}

} // namespace tlr
