/**
 * TypeScript enums mirroring C++ enums from src/DataModelDef.h
 */

export enum OrderType {
  MARKET = 'MARKET',
  LIMIT = 'LIMIT',
  STOP = 'STOP',
  STOPLIMIT = 'STOPLIMIT',
}

export enum Side {
  BUY = 'BUY',
  SELL = 'SELL',
  BUY_MINUS = 'BUY_MINUS',
  SELL_PLUS = 'SELL_PLUS',
  SELL_SHORT = 'SELL_SHORT',
  CROSS = 'CROSS',
}

export enum TimeInForce {
  DAY = 'DAY',
  GTD = 'GTD',
  GTC = 'GTC',
  FOK = 'FOK',
  IOC = 'IOC',
  OPG = 'OPG',
  ATCLOSE = 'ATCLOSE',
}

export enum OrderStatus {
  RECEIVED_NEW = 'RECEIVED_NEW',
  PENDING_NEW = 'PENDING_NEW',
  PENDING_REPLACE = 'PENDING_REPLACE',
  NEW = 'NEW',
  PARTIAL_FILL = 'PARTIAL_FILL',
  FILLED = 'FILLED',
  EXPIRED = 'EXPIRED',
  DONE_FOR_DAY = 'DONE_FOR_DAY',
  SUSPENDED = 'SUSPENDED',
  REPLACED = 'REPLACED',
  CANCELED = 'CANCELED',
  REJECTED = 'REJECTED',
}

export enum ExecType {
  NEW = 'NEW',
  TRADE = 'TRADE',
  DFD = 'DFD',
  CORRECT = 'CORRECT',
  CANCEL = 'CANCEL',
  REJECT = 'REJECT',
  REPLACE = 'REPLACE',
  EXPIRED = 'EXPIRED',
  SUSPENDED = 'SUSPENDED',
  STATUS = 'STATUS',
  RESTATED = 'RESTATED',
  PEND_CANCEL = 'PEND_CANCEL',
  PEND_REPLACE = 'PEND_REPLACE',
}

export enum Currency {
  USD = 'USD',
  EUR = 'EUR',
}

export enum Capacity {
  AGENCY = 'AGENCY',
  PRINCIPAL = 'PRINCIPAL',
  PROPRIETARY = 'PROPRIETARY',
  INDIVIDUAL = 'INDIVIDUAL',
  RISKLESS_PRINCIPAL = 'RISKLESS_PRINCIPAL',
  AGENT_FOR_ANOTHER_MEMBER = 'AGENT_FOR_ANOTHER_MEMBER',
}

export enum AccountType {
  PRINCIPAL = 'PRINCIPAL',
  AGENCY = 'AGENCY',
}
