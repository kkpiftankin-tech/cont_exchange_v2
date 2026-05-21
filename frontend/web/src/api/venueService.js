import axios from 'axios';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 10000);

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

export const getVenues = async () => {
  const response = await api.get('/venues');
  return response.data;
};

export const getVenueById = async (venueId) => {
  const response = await api.get(`/venues/${encodeURIComponent(venueId)}`);
  return response.data;
};

export const getVenueCurves = async (venueId, limit = 1) => {
  const response = await api.get(`/venues/${encodeURIComponent(venueId)}/curves`, {
    params: { limit },
  });
  return response.data;
};

export const getVenueSnapshots = async (venueId) => {
  const response = await api.get(`/venues/${encodeURIComponent(venueId)}/snapshots`);
  return response.data;
};

export const getVenueSynthetics = async (venueId) => {
  const response = await api.get(`/venues/${encodeURIComponent(venueId)}/synthetics`);
  return response.data;
};

export const reconnectVenue = async (venueId) => {
  const response = await api.post(`/venues/${encodeURIComponent(venueId)}/reconnect`);
  return response.data;
};

export const disableVenue = async (venueId) => {
  const response = await api.post(`/venues/${encodeURIComponent(venueId)}/disable`);
  return response.data;
};

export const enableVenue = async (venueId) => {
  const response = await api.post(`/venues/${encodeURIComponent(venueId)}/enable`);
  return response.data;
};

export const updateVenueRoutingMode = async (venueId, mode) => {
  const response = await api.post(`/venues/${encodeURIComponent(venueId)}/routing-mode`, { mode });
  return response.data;
};

export const upsertVenueConfig = async (venueId, payload) => {
  const response = await api.put(`/venues/${encodeURIComponent(venueId)}/config`, payload);
  return response.data;
};

export const createVenueConfig = async (payload) => {
  const response = await api.post('/venues', payload);
  return response.data;
};

export const deleteVenueConfig = async (venueId) => {
  const response = await api.delete(`/venues/${encodeURIComponent(venueId)}/config`);
  return response.data;
};
