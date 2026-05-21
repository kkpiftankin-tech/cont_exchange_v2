import axios from 'axios';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 10000);

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

export const getBatches = async () => {
  const response = await api.get('/batches');
  return response.data?.items || [];
};

