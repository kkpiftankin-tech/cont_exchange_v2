import React from 'react';
import { useTranslation } from 'react-i18next';

const VenueConfigFields = ({
  draft,
  onPatch,
  disabled = false,
  includeVenueId = false,
}) => {
  const { t } = useTranslation();

  const patchText = (key) => (event) => onPatch(key, event.target.value);
  const patchNumber = (key) => (event) => onPatch(key, Number(event.target.value || 0));
  const patchChecked = (key) => (event) => onPatch(key, event.target.checked);
  const withHelp = (field, input, { checkbox = false, readOnly = false } = {}) => (
    <label
      className={`venues-form-field${checkbox ? ' venues-checkbox-field' : ''}${readOnly ? ' venues-form-field-readonly' : ''}`}
    >
      <span className="venues-form-title">{t(`venues.operator.fields.${field}`)}</span>
      <small className="venues-form-help">{t(`venues.operator.help.${field}`)}</small>
      {input}
    </label>
  );

  return (
    <div className="venues-form-grid">
      {includeVenueId ? (
        withHelp(
          'venueId',
          <input value={draft.venueId} disabled={disabled} onChange={patchText('venueId')} />
        )
      ) : null}

      {withHelp(
        'adapterMode',
        <input value={draft.adapterMode} disabled readOnly onChange={patchText('adapterMode')} />,
        { readOnly: true }
      )}

      {withHelp(
        'venueSymbol',
        <input value={draft.venueSymbol} disabled={disabled} onChange={patchText('venueSymbol')} />
      )}

      {withHelp(
        'wsUrl',
        <input value={draft.wsUrl} disabled={disabled} onChange={patchText('wsUrl')} />
      )}

      {withHelp(
        'restBaseUrl',
        <input value={draft.restBaseUrl} disabled={disabled} onChange={patchText('restBaseUrl')} />
      )}

      {withHelp(
        'rpcUrl',
        <input value={draft.rpcUrl} disabled={disabled} onChange={patchText('rpcUrl')} />
      )}

      {withHelp(
        'chainId',
        <input value={draft.chainId} disabled={disabled} onChange={patchText('chainId')} />
      )}

      {withHelp(
        'poolAddress',
        <input value={draft.poolAddress} disabled={disabled} onChange={patchText('poolAddress')} />
      )}

      {withHelp(
        'curveLevel',
        <select value={draft.curveLevel} disabled={disabled} onChange={patchText('curveLevel')}>
          <option value="L1">L1</option>
          <option value="L2">L2</option>
          <option value="L3">L3</option>
          <option value="OFF">OFF</option>
        </select>
      )}

      {withHelp(
        'routingMode',
        <select value={draft.routingMode} disabled={disabled} onChange={patchText('routingMode')}>
          <option value="auto">{t('venues.operator.mode.auto')}</option>
          <option value="watch">{t('venues.operator.mode.watch')}</option>
        </select>
      )}

      {withHelp(
        'depthLevels',
        <input type="number" min={1} value={draft.depthLevels} disabled={disabled} onChange={patchNumber('depthLevels')} />
      )}

      {withHelp(
        'staleThresholdMs',
        <input
          type="number"
          min={1}
          value={draft.staleThresholdMs}
          disabled={disabled}
          onChange={patchNumber('staleThresholdMs')}
        />
      )}

      {withHelp(
        'circuitBreakerErrors',
        <input
          type="number"
          min={1}
          value={draft.circuitBreakerErrors}
          disabled={disabled}
          onChange={patchNumber('circuitBreakerErrors')}
        />
      )}

      {withHelp(
        'circuitBreakerWindowMs',
        <input
          type="number"
          min={1}
          value={draft.circuitBreakerWindowMs}
          disabled={disabled}
          onChange={patchNumber('circuitBreakerWindowMs')}
        />
      )}

      {withHelp(
        'circuitBreakerCooldownMs',
        <input
          type="number"
          min={1}
          value={draft.circuitBreakerCooldownMs}
          disabled={disabled}
          onChange={patchNumber('circuitBreakerCooldownMs')}
        />
      )}

      {withHelp(
        'syntheticEnabled',
        <input type="checkbox" checked={draft.syntheticEnabled} disabled={disabled} onChange={patchChecked('syntheticEnabled')} />,
        { checkbox: true }
      )}

      {withHelp(
        'circuitBreakerEnabled',
        <input
          type="checkbox"
          checked={draft.circuitBreakerEnabled}
          disabled={disabled}
          onChange={patchChecked('circuitBreakerEnabled')}
        />,
        { checkbox: true }
      )}

      {withHelp(
        'isActive',
        <input type="checkbox" checked={draft.isActive} disabled={disabled} onChange={patchChecked('isActive')} />,
        { checkbox: true }
      )}
    </div>
  );
};

export default VenueConfigFields;
