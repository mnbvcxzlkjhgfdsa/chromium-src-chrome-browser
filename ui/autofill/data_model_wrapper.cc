// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autofill/data_model_wrapper.h"

#include "base/callback.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/ui/autofill/autofill_dialog_common.h"
#include "chrome/browser/ui/autofill/autofill_dialog_models.h"
#include "components/autofill/content/browser/wallet/full_wallet.h"
#include "components/autofill/content/browser/wallet/wallet_address.h"
#include "components/autofill/content/browser/wallet/wallet_items.h"
#include "components/autofill/core/browser/autofill_data_model.h"
#include "components/autofill/core/browser/autofill_field.h"
#include "components/autofill/core/browser/autofill_profile.h"
#include "components/autofill/core/browser/autofill_type.h"
#include "components/autofill/core/browser/credit_card.h"
#include "components/autofill/core/browser/form_structure.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image.h"

#if !defined(OS_ANDROID)
#include "third_party/libaddressinput/chromium/cpp/include/libaddressinput/address_data.h"
#include "third_party/libaddressinput/chromium/cpp/include/libaddressinput/address_ui.h"
#endif

namespace autofill {

using base::ASCIIToUTF16;
using base::UTF16ToUTF8;

DataModelWrapper::~DataModelWrapper() {}

void DataModelWrapper::FillInputs(DetailInputs* inputs) {
  for (size_t i = 0; i < inputs->size(); ++i) {
    DetailInput* input = &(*inputs)[i];
    input->initial_value = common::GetHardcodedValueForType(input->type);
    if (input->initial_value.empty())
      input->initial_value = GetInfo(AutofillType(input->type));
  }
}

base::string16 DataModelWrapper::GetInfoForDisplay(const AutofillType& type)
    const {
  return GetInfo(type);
}

gfx::Image DataModelWrapper::GetIcon() {
  return gfx::Image();
}

#if !defined(OS_ANDROID)
bool DataModelWrapper::GetDisplayText(
    base::string16* vertically_compact,
    base::string16* horizontally_compact) {
  // Format the address.
  ::i18n::addressinput::AddressData address_data;
  address_data.recipient = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(NAME_FULL)));
  address_data.country_code = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_COUNTRY)));
  address_data.administrative_area = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_STATE)));
  address_data.locality = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_CITY)));
  address_data.dependent_locality = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_DEPENDENT_LOCALITY)));
  address_data.sorting_code = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_SORTING_CODE)));
  address_data.postal_code = UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_ZIP)));

  address_data.address_lines.push_back(UTF16ToUTF8(
      GetInfoForDisplay(AutofillType(ADDRESS_HOME_LINE1))));
  base::string16 address2 = GetInfoForDisplay(AutofillType(ADDRESS_HOME_LINE2));
  if (!address2.empty())
    address_data.address_lines.push_back(UTF16ToUTF8(address2));

  std::vector<std::string> lines;
  address_data.FormatForDisplay(&lines);

  // Email and phone number aren't part of address formatting.
  base::string16 non_address_info;
  base::string16 email = GetInfoForDisplay(AutofillType(EMAIL_ADDRESS));
  if (!email.empty())
    non_address_info += ASCIIToUTF16("\n") + email;

  non_address_info += ASCIIToUTF16("\n") +
      GetInfoForDisplay(AutofillType(PHONE_HOME_WHOLE_NUMBER));

  // The separator is locale-specific.
  std::string compact_separator =
      ::i18n::addressinput::GetCompactAddressLinesSeparator(
          g_browser_process->GetApplicationLocale());
  *vertically_compact =
      base::UTF8ToUTF16(JoinString(lines, compact_separator)) +
          non_address_info;
  *horizontally_compact = base::UTF8ToUTF16(JoinString(lines, "\n")) +
      non_address_info;

  return true;
}
#endif

bool DataModelWrapper::FillFormStructure(
    const DetailInputs& inputs,
    const InputFieldComparator& compare,
    FormStructure* form_structure) const {
  bool filled_something = false;
  for (size_t i = 0; i < form_structure->field_count(); ++i) {
    AutofillField* field = form_structure->field(i);
    for (size_t j = 0; j < inputs.size(); ++j) {
      if (compare.Run(inputs[j].type, *field)) {
        AutofillField::FillFormField(*field, GetInfo(field->Type()),
                                     g_browser_process->GetApplicationLocale(),
                                     field);
        filled_something = true;
        break;
      }
    }
  }
  return filled_something;
}

DataModelWrapper::DataModelWrapper() {}

// EmptyDataModelWrapper

EmptyDataModelWrapper::EmptyDataModelWrapper() {}
EmptyDataModelWrapper::~EmptyDataModelWrapper() {}

base::string16 EmptyDataModelWrapper::GetInfo(const AutofillType& type) const {
  return base::string16();
}

// AutofillProfileWrapper

AutofillProfileWrapper::AutofillProfileWrapper(const AutofillProfile* profile)
    : profile_(profile),
      variant_group_(NO_GROUP),
      variant_(0) {}

AutofillProfileWrapper::AutofillProfileWrapper(
    const AutofillProfile* profile,
    const AutofillType& type,
    size_t variant)
    : profile_(profile),
      variant_group_(type.group()),
      variant_(variant) {}

AutofillProfileWrapper::~AutofillProfileWrapper() {}

base::string16 AutofillProfileWrapper::GetInfo(const AutofillType& type) const {
  // Requests for the user's credit card are filled from the billing address,
  // but the AutofillProfile class doesn't know how to fill credit card
  // fields. So, request for the corresponding profile type instead.
  AutofillType effective_type = type;
  if (type.GetStorableType() == CREDIT_CARD_NAME)
    effective_type = AutofillType(NAME_BILLING_FULL);

  size_t variant = GetVariantForType(effective_type);
  const std::string& app_locale = g_browser_process->GetApplicationLocale();
  return profile_->GetInfoForVariant(effective_type, variant, app_locale);
}

base::string16 AutofillProfileWrapper::GetInfoForDisplay(
    const AutofillType& type) const {
  // We display the "raw" phone number which contains user-defined formatting.
  if (type.GetStorableType() == PHONE_HOME_WHOLE_NUMBER) {
    std::vector<base::string16> values;
    profile_->GetRawMultiInfo(type.GetStorableType(), &values);
    const base::string16& phone_number = values[GetVariantForType(type)];

    // If there is no user-defined formatting at all, add some standard
    // formatting.
    if (ContainsOnlyChars(phone_number, ASCIIToUTF16("0123456789"))) {
      std::string region = UTF16ToASCII(
          GetInfo(AutofillType(HTML_TYPE_COUNTRY_CODE, HTML_MODE_NONE)));
      i18n::PhoneObject phone(phone_number, region);
      return phone.GetFormattedNumber();
    }

    return phone_number;
  }

  return DataModelWrapper::GetInfoForDisplay(type);
}

size_t AutofillProfileWrapper::GetVariantForType(const AutofillType& type)
    const {
  if (type.group() == variant_group_)
    return variant_;

  return 0;
}

// AutofillShippingAddressWrapper

AutofillShippingAddressWrapper::AutofillShippingAddressWrapper(
    const AutofillProfile* profile)
    : AutofillProfileWrapper(profile) {}

AutofillShippingAddressWrapper::~AutofillShippingAddressWrapper() {}

base::string16 AutofillShippingAddressWrapper::GetInfo(
    const AutofillType& type) const {
  // Shipping addresses don't have email addresses associated with them.
  if (type.GetStorableType() == EMAIL_ADDRESS)
    return base::string16();

  return AutofillProfileWrapper::GetInfo(type);
}

// AutofillCreditCardWrapper

AutofillCreditCardWrapper::AutofillCreditCardWrapper(const CreditCard* card)
    : card_(card) {}

AutofillCreditCardWrapper::~AutofillCreditCardWrapper() {}

base::string16 AutofillCreditCardWrapper::GetInfo(const AutofillType& type)
    const {
  if (type.group() != CREDIT_CARD)
    return base::string16();

  if (type.GetStorableType() == CREDIT_CARD_EXP_MONTH)
    return MonthComboboxModel::FormatMonth(card_->expiration_month());

  return card_->GetInfo(type, g_browser_process->GetApplicationLocale());
}

gfx::Image AutofillCreditCardWrapper::GetIcon() {
  ui::ResourceBundle& rb = ui::ResourceBundle::GetSharedInstance();
  return rb.GetImageNamed(CreditCard::IconResourceId(card_->type()));
}

#if !defined(OS_ANDROID)
bool AutofillCreditCardWrapper::GetDisplayText(
    base::string16* vertically_compact,
    base::string16* horizontally_compact) {
  if (!card_->IsValid())
    return false;

  *vertically_compact = *horizontally_compact = card_->TypeAndLastFourDigits();
  return true;
}
#endif

// WalletAddressWrapper

WalletAddressWrapper::WalletAddressWrapper(
    const wallet::Address* address) : address_(address) {}

WalletAddressWrapper::~WalletAddressWrapper() {}

base::string16 WalletAddressWrapper::GetInfo(const AutofillType& type) const {
  // Reachable from DataModelWrapper::GetDisplayText().
  if (type.GetStorableType() == EMAIL_ADDRESS)
    return base::string16();

  return address_->GetInfo(type, g_browser_process->GetApplicationLocale());
}

base::string16 WalletAddressWrapper::GetInfoForDisplay(const AutofillType& type)
    const {
  if (type.GetStorableType() == PHONE_HOME_WHOLE_NUMBER)
    return address_->DisplayPhoneNumber();

  return DataModelWrapper::GetInfoForDisplay(type);
}

#if !defined(OS_ANDROID)
bool WalletAddressWrapper::GetDisplayText(
    base::string16* vertically_compact,
    base::string16* horizontally_compact) {
  if (!address_->is_complete_address() ||
      GetInfo(AutofillType(PHONE_HOME_WHOLE_NUMBER)).empty()) {
    return false;
  }

  return DataModelWrapper::GetDisplayText(vertically_compact,
                                          horizontally_compact);
}
#endif

// WalletInstrumentWrapper

WalletInstrumentWrapper::WalletInstrumentWrapper(
    const wallet::WalletItems::MaskedInstrument* instrument)
    : instrument_(instrument) {}

WalletInstrumentWrapper::~WalletInstrumentWrapper() {}

base::string16 WalletInstrumentWrapper::GetInfo(const AutofillType& type)
    const {
  // Reachable from DataModelWrapper::GetDisplayText().
  if (type.GetStorableType() == EMAIL_ADDRESS)
    return base::string16();

  if (type.GetStorableType() == CREDIT_CARD_EXP_MONTH)
    return MonthComboboxModel::FormatMonth(instrument_->expiration_month());

  return instrument_->GetInfo(type, g_browser_process->GetApplicationLocale());
}

base::string16 WalletInstrumentWrapper::GetInfoForDisplay(
    const AutofillType& type) const {
  if (type.GetStorableType() == PHONE_HOME_WHOLE_NUMBER)
    return instrument_->address().DisplayPhoneNumber();

  return DataModelWrapper::GetInfoForDisplay(type);
}

gfx::Image WalletInstrumentWrapper::GetIcon() {
  return instrument_->CardIcon();
}

#if !defined(OS_ANDROID)
bool WalletInstrumentWrapper::GetDisplayText(
    base::string16* vertically_compact,
    base::string16* horizontally_compact) {
  // TODO(dbeam): handle other instrument statuses? http://crbug.com/233048
  if (instrument_->status() == wallet::WalletItems::MaskedInstrument::EXPIRED ||
      !instrument_->address().is_complete_address() ||
      GetInfo(AutofillType(PHONE_HOME_WHOLE_NUMBER)).empty()) {
    return false;
  }

  DataModelWrapper::GetDisplayText(vertically_compact, horizontally_compact);
  // TODO(estade): descriptive_name() is user-provided. Should we use it or
  // just type + last 4 digits?
  base::string16 line1 = instrument_->descriptive_name() + ASCIIToUTF16("\n");
  *vertically_compact = line1 + *vertically_compact;
  *horizontally_compact = line1 + *horizontally_compact;
  return true;
}
#endif

// FullWalletBillingWrapper

FullWalletBillingWrapper::FullWalletBillingWrapper(
    wallet::FullWallet* full_wallet)
    : full_wallet_(full_wallet) {
  DCHECK(full_wallet_);
}

FullWalletBillingWrapper::~FullWalletBillingWrapper() {}

base::string16 FullWalletBillingWrapper::GetInfo(const AutofillType& type)
    const {
  if (type.GetStorableType() == CREDIT_CARD_EXP_MONTH)
    return MonthComboboxModel::FormatMonth(full_wallet_->expiration_month());

  if (type.group() == CREDIT_CARD)
    return full_wallet_->GetInfo(type);

  return full_wallet_->billing_address()->GetInfo(
      type, g_browser_process->GetApplicationLocale());
}

#if !defined(OS_ANDROID)
bool FullWalletBillingWrapper::GetDisplayText(
    base::string16* vertically_compact,
    base::string16* horizontally_compact) {
  // TODO(dbeam): handle other required actions? http://crbug.com/163508
  if (full_wallet_->HasRequiredAction(wallet::UPDATE_EXPIRATION_DATE))
    return false;

  return DataModelWrapper::GetDisplayText(vertically_compact,
                                          horizontally_compact);
}
#endif

// FullWalletShippingWrapper

FullWalletShippingWrapper::FullWalletShippingWrapper(
    wallet::FullWallet* full_wallet)
    : full_wallet_(full_wallet) {
  DCHECK(full_wallet_);
}

FullWalletShippingWrapper::~FullWalletShippingWrapper() {}

base::string16 FullWalletShippingWrapper::GetInfo(
    const AutofillType& type) const {
  return full_wallet_->shipping_address()->GetInfo(
      type, g_browser_process->GetApplicationLocale());
}

FieldMapWrapper::FieldMapWrapper(const FieldValueMap& field_map)
    : field_map_(field_map) {}

FieldMapWrapper::~FieldMapWrapper() {}

base::string16 FieldMapWrapper::GetInfo(const AutofillType& type) const {
  FieldValueMap::const_iterator it = field_map_.find(type.server_type());
  return it != field_map_.end() ? it->second : base::string16();
}

}  // namespace autofill
